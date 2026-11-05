#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include "Network_Maro.h"
#include "Campaign_Maro.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace maro {
namespace {

using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using Byte = std::uint8_t;

constexpr auto actionInterval = std::chrono::milliseconds(140);
constexpr auto heartbeat = std::chrono::seconds(1);
constexpr auto handshakeLimit = std::chrono::seconds(5);
constexpr auto idleLimit = std::chrono::seconds(8);
constexpr std::size_t maxFrame = 64;
constexpr std::size_t maxQueue = 8192;
constexpr Byte version = 2;

enum Message : Byte { Hello = 1, Move, Interact, Ping, Snapshot, Reject, Answer };
enum Reason : Byte { Full = 1, InProgress, WrongVersion };

bool same(Point first, Point second) {
    return first.x == second.x && first.y == second.y;
}

char mapChar(int day, int x, int y) {
    return campaign[day - 1].map[y][x];
}

Point positionOf(int day, char marker) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (mapChar(day, x, y) == marker) {
                return {x, y};
            }
        }
    }
    return {0, 0};
}

Point spawnFor(int day, int id) {
    return positionOf(day, static_cast<char>('1' + id));
}

int orderCount(const State& state) {
    return campaign[state.day - 1].orderCount;
}

std::chrono::milliseconds moveInterval(const State& state) {
    return std::chrono::milliseconds(90 - 15 * state.speedLevel);
}

bool cardinal(int dx, int dy) {
    return ((dx == 1 || dx == -1) && dy == 0)
        || ((dy == 1 || dy == -1) && dx == 0);
}

bool validPort(int value) {
    return value >= 1 && value <= 65535;
}

bool parseAddress(const std::string& input, sockaddr_in& address) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || input.find('\0') != std::string::npos) {
        return false;
    }
    const auto last = input.find_last_not_of(" \t\r\n");
    const std::string endpoint = input.substr(first, last - first + 1);
    const auto delimiter = endpoint.find(':');
    const std::string ipv4 = endpoint.substr(0, delimiter);
    int endpointPort = port;
    if (delimiter != std::string::npos) {
        const auto count = endpoint.size() - delimiter - 1;
        if (count == 0 || count > 5) {
            return false;
        }
        endpointPort = 0;
        for (std::size_t index = delimiter + 1; index < endpoint.size(); ++index) {
            if (endpoint[index] < '0' || endpoint[index] > '9') {
                return false;
            }
            endpointPort = endpointPort * 10 + (endpoint[index] - '0');
        }
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(endpointPort));
    return validPort(endpointPort) && InetPtonA(AF_INET, ipv4.c_str(), &address.sin_addr) == 1;
}

bool occupied(const State& state, Point pos) {
    return std::any_of(state.players.begin(), state.players.end(), [pos](const Player& player) {
        return player.active && same(player.pos, pos);
    });
}

int playerCount(const State& state) {
    return static_cast<int>(std::count_if(state.players.begin(), state.players.end(), [](const Player& player) {
        return player.active;
    }));
}

bool allDelivered(const State& state) {
    return std::all_of(state.parcels.begin(), state.parcels.begin() + orderCount(state), [](const Parcel& parcel) {
        return parcel.stage == OrderStage::Delivered;
    });
}

int firstOrder(const State& state, OrderStage stage) {
    const auto end = state.parcels.begin() + orderCount(state);
    const auto order = std::find_if(state.parcels.begin(), end, [stage](const Parcel& parcel) {
        return parcel.stage == stage;
    });
    return order == end ? -1 : static_cast<int>(order - state.parcels.begin());
}

bool platesPressed(const State& state) {
    return std::count_if(state.players.begin(), state.players.end(), [&](const Player& player) {
        return player.active && tileAt(state.day, player.pos.x, player.pos.y) == Tile::Switch;
    }) == 2;
}

State newState(Phase phase, int day = 1) {
    State result{};
    result.phase = phase;
    result.day = day;
    for (int index = 0; index < maxPlayers; ++index) {
        result.players[index] = {false, spawnFor(day, index), -1, -1};
    }
    const Point terminal = positionOf(day, 'O');
    for (Parcel& parcel : result.parcels) {
        parcel = {terminal, -1, OrderStage::Queued};
    }
    return result;
}

struct Channel {
    SOCKET socket = INVALID_SOCKET;
    std::vector<Byte> incoming;
    std::vector<Byte> outgoing;
    Time lastPacket{};
    int id = -1;
    bool closeAfterSend = false;
};

void closeChannel(Channel& channel) {
    if (channel.socket != INVALID_SOCKET) {
        closesocket(channel.socket);
    }
    channel = Channel{};
}

bool nonblocking(SOCKET socket) {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

void configureStream(SOCKET socket) {
    BOOL enabled = TRUE;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
}

bool enqueue(Channel& channel, const Byte* bytes, std::size_t count) {
    if (count == 0 || count > maxFrame || channel.outgoing.size() + count + 2 > maxQueue) {
        return false;
    }
    channel.outgoing.push_back(static_cast<Byte>(count >> 8));
    channel.outgoing.push_back(static_cast<Byte>(count));
    channel.outgoing.insert(channel.outgoing.end(), bytes, bytes + count);
    return true;
}

bool enqueue(Channel& channel, std::initializer_list<Byte> bytes) {
    return enqueue(channel, bytes.begin(), bytes.size());
}

bool transientError() {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
}

bool flush(Channel& channel) {
    if (channel.outgoing.empty()) {
        return true;
    }
    const int sent = send(channel.socket, reinterpret_cast<const char*>(channel.outgoing.data()),
                          static_cast<int>(channel.outgoing.size()), 0);
    if (sent == SOCKET_ERROR) {
        return transientError();
    }
    channel.outgoing.erase(channel.outgoing.begin(), channel.outgoing.begin() + sent);
    return sent > 0;
}

template <typename OnFrame>
bool receive(Channel& channel, Time now, OnFrame onFrame) {
    Byte buffer[512];
    for (int read = 0; read < 8; ++read) {
        const int count = recv(channel.socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (count == SOCKET_ERROR) {
            return transientError();
        }
        if (count == 0) {
            return false;
        }

        auto& incoming = channel.incoming;
        incoming.insert(incoming.end(), buffer, buffer + count);
        std::size_t offset = 0;
        while (incoming.size() - offset >= 2) {
            const std::size_t frameSize = (static_cast<std::size_t>(incoming[offset]) << 8)
                | incoming[offset + 1];
            if (frameSize == 0 || frameSize > maxFrame) {
                return false;
            }
            if (incoming.size() - offset - 2 < frameSize) {
                break;
            }
            if (!onFrame(incoming.data() + offset + 2, frameSize)) {
                return false;
            }
            channel.lastPacket = now;
            offset += frameSize + 2;
            if (channel.closeAfterSend) {
                incoming.clear();
                return true;
            }
        }
        incoming.erase(incoming.begin(), incoming.begin() + offset);
    }
    return true;
}

constexpr std::size_t snapshotHeader = 9;
constexpr std::size_t snapshotSize = snapshotHeader + maxPlayers * 5 + parcelCount * 4;
static_assert(snapshotSize <= maxFrame, "Snapshot exceeds maximum frame size");

std::array<Byte, snapshotSize> encodeSnapshot(const State& state, int id, Byte round) {
    std::array<Byte, snapshotSize> bytes{};
    auto writer = bytes.begin();
    *writer++ = Snapshot;
    *writer++ = static_cast<Byte>(state.phase);
    *writer++ = static_cast<Byte>(state.gateOpen);
    *writer++ = static_cast<Byte>(id);
    *writer++ = round;
    *writer++ = static_cast<Byte>(state.day);
    *writer++ = static_cast<Byte>(state.speedLevel);
    *writer++ = static_cast<Byte>(state.bufferLevel);
    *writer++ = static_cast<Byte>(state.scanner);
    for (const Player& player : state.players) {
        *writer++ = static_cast<Byte>(player.active);
        *writer++ = static_cast<Byte>(player.pos.x);
        *writer++ = static_cast<Byte>(player.pos.y);
        *writer++ = static_cast<Byte>(player.carrying + 1);
        *writer++ = static_cast<Byte>(player.lastWrongOrder + 1);
    }
    for (const Parcel& parcel : state.parcels) {
        *writer++ = static_cast<Byte>(parcel.pos.x);
        *writer++ = static_cast<Byte>(parcel.pos.y);
        *writer++ = static_cast<Byte>(parcel.holder + 1);
        *writer++ = static_cast<Byte>(parcel.stage);
    }
    return bytes;
}

bool decodeSnapshot(const Byte* data, std::size_t size, State& state, int& id) {
    if (size != snapshotSize || data[1] < static_cast<Byte>(Phase::Lobby)
        || data[1] > static_cast<Byte>(Phase::Won) || data[2] > 1 || data[3] >= maxPlayers
        || data[5] < 1 || data[5] > campaignDays || data[6] > 3 || data[7] > 3 || data[8] > 1
        || data[6] + data[7] + data[8] > (data[5] - 1) / 3) {
        return false;
    }
    if (id != -1 && id != data[3]) {
        return false;
    }

    State next = newState(static_cast<Phase>(data[1]), data[5]);
    next.gateOpen = data[2] != 0;
    next.speedLevel = data[6];
    next.bufferLevel = data[7];
    next.scanner = data[8] != 0;
    const int orders = orderCount(next);
    const Byte* reader = data + snapshotHeader;
    for (Player& player : next.players) {
        const Point pos{reader[1], reader[2]};
        if (reader[0] > 1 || reader[3] > orders || reader[4] > orders
            || tileAt(next.day, pos.x, pos.y) == Tile::Wall
            || (reader[0] != 0 && occupied(next, pos))
            || (reader[0] == 0 && (reader[3] != 0 || reader[4] != 0))) {
            return false;
        }
        player = {reader[0] != 0, pos, reader[3] - 1, reader[4] - 1};
        reader += 5;
    }
    for (int index = 0; index < parcelCount; ++index) {
        const Point pos{reader[0], reader[1]};
        const Tile tile = tileAt(next.day, pos.x, pos.y);
        const auto stage = static_cast<OrderStage>(reader[3]);
        if (reader[2] > maxPlayers || stage > OrderStage::Delivered || tile == Tile::Wall
            || (reader[2] != 0 && stage != OrderStage::Packed)
            || (stage == OrderStage::Delivered && tile != Tile::Destination)
            || (stage < OrderStage::Packed && tile != Tile::Terminal)
            || (index >= orders && stage != OrderStage::Queued)) {
            return false;
        }
        next.parcels[index] = {pos, reader[2] - 1, stage};
        reader += 4;
    }
    for (int index = 0; index < maxPlayers; ++index) {
        const Player& player = next.players[index];
        if (player.carrying >= 0 && next.parcels[player.carrying].holder != index) {
            return false;
        }
        if (player.lastWrongOrder >= 0 && next.parcels[player.lastWrongOrder].stage != OrderStage::Ordered) {
            return false;
        }
    }
    for (int index = 0; index < parcelCount; ++index) {
        const Parcel& parcel = next.parcels[index];
        if (parcel.holder >= 0) {
            const Player& holder = next.players[parcel.holder];
            if (!holder.active || holder.carrying != index || !same(holder.pos, parcel.pos)) {
                return false;
            }
        }
    }
    const bool pressed = platesPressed(next);
    const bool completed = next.phase == Phase::DayDone || next.phase == Phase::Upgrade || next.phase == Phase::Won;
    if (!next.players[data[3]].active || !next.players[0].active || (pressed && !next.gateOpen)
        || (next.gateOpen && !pressed && next.bufferLevel == 0)
        || (next.phase != Phase::Lobby && (playerCount(next) < minPlayers || completed != allDelivered(next)))
        || (next.phase == Phase::Won && next.day != campaignDays)
        || (next.phase == Phase::Upgrade && (next.day == campaignDays || next.day % 3 != 0))
        || (next.phase == Phase::DayDone && next.day % 3 == 0)) {
        return false;
    }
    id = data[3];
    state = next;
    return true;
}

const char* rejection(Byte reason) {
    switch (reason) {
    case Full: return u8"방에 이미 6명이 있습니다.";
    case InProgress: return u8"이미 배달 중인 방입니다. 다음 판에 접속해 주세요.";
    case WrongVersion: return u8"호스트와 게임 버전이 다릅니다.";
    default: return u8"호스트가 연결을 거절했습니다.";
    }
}

}

Tile tileAt(int day, int x, int y) {
    if (day < 1 || day > campaignDays || x < 0 || y < 0 || x >= width || y >= height) {
        return Tile::Wall;
    }
    switch (mapChar(day, x, y)) {
    case '#': return Tile::Wall;
    case 'G': return Tile::Gate;
    case 'S': return Tile::Switch;
    case 'D': return Tile::Destination;
    case 'O': return Tile::Terminal;
    case 'P': return Tile::Packing;
    default: return Tile::Floor;
    }
}

struct Net::Impl {
    Impl() {
        WSADATA info{};
        winsock = WSAStartup(MAKEWORD(2, 2), &info) == 0;
    }

    ~Impl() {
        reset();
        if (winsock) {
            WSACleanup();
        }
    }

    bool winsock = false;
    SOCKET listener = INVALID_SOCKET;
    std::array<Channel, maxPlayers + 2> peers;
    Channel server;
    bool dialing = false;
    bool dirty = false;
    bool platesHeld = false;
    Byte round = 0;
    int id = -1;
    int boundPort = port;
    Phase resumePhase = Phase::Story;
    State state = newState(Phase::Disconnected);
    std::string notice;
    Time lastSnapshot{};
    Time lastPing{};
    Time gateUntil{};
    std::array<Time, maxPlayers> lastMoves{};
    std::array<Time, maxPlayers> lastActions{};

    void reset() {
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
            listener = INVALID_SOCKET;
        }
        for (Channel& peer : peers) {
            closeChannel(peer);
        }
        closeChannel(server);
        state = newState(Phase::Disconnected);
        lastMoves = {};
        lastActions = {};
        lastSnapshot = {};
        lastPing = {};
        gateUntil = {};
        dialing = false;
        dirty = false;
        platesHeld = false;
        round = 0;
        id = -1;
        boundPort = port;
        resumePhase = Phase::Story;
    }

    void report(const char* message) {
        reset();
        notice = message;
    }

    void resetBoard(Phase phase, int day, bool keepUpgrades = true) {
        State next = newState(phase, day);
        for (int index = 0; index < maxPlayers; ++index) {
            next.players[index].active = state.players[index].active;
        }
        if (keepUpgrades) {
            next.speedLevel = state.speedLevel;
            next.bufferLevel = state.bufferLevel;
            next.scanner = state.scanner;
        }
        state = next;
        lastMoves = {};
        lastActions = {};
        gateUntil = {};
        platesHeld = false;
        dirty = true;
    }

    void refreshGate(Time now) {
        const bool pressed = platesPressed(state);
        if (platesHeld && !pressed) {
            gateUntil = now + std::chrono::milliseconds(350 * state.bufferLevel);
        }
        platesHeld = pressed;
        const bool open = pressed || now < gateUntil;
        if (open != state.gateOpen) {
            state.gateOpen = open;
            dirty = true;
        }
    }

    void retire(Channel& peer, Time now) {
        const int leaving = peer.id;
        closeChannel(peer);
        if (leaving < 0) {
            return;
        }
        Player& player = state.players[leaving];
        if (player.carrying >= 0) {
            state.parcels[player.carrying].holder = -1;
        }
        player = {false, spawnFor(state.day, leaving), -1, -1};
        lastMoves[leaving] = {};
        lastActions[leaving] = {};
        if (state.phase != Phase::Lobby && playerCount(state) < minPlayers) {
            if (state.phase == Phase::Playing || state.phase == Phase::Story) {
                resumePhase = Phase::Story;
                resetBoard(Phase::Lobby, state.day);
            } else {
                resumePhase = state.phase;
                state.phase = Phase::Lobby;
            }
            notice = u8"3명 미만이 되어 대기실로 돌아왔습니다.";
        }
        refreshGate(now);
        dirty = true;
    }

    void movePlayer(int playerId, int dx, int dy, Time now) {
        if (state.phase != Phase::Playing || now - lastMoves[playerId] < moveInterval(state)) {
            return;
        }
        lastMoves[playerId] = now;
        Player& player = state.players[playerId];
        const Point next{player.pos.x + dx, player.pos.y + dy};
        const Tile tile = tileAt(state.day, next.x, next.y);
        if (tile == Tile::Wall || (tile == Tile::Gate && !state.gateOpen) || occupied(state, next)) {
            return;
        }
        player.pos = next;
        if (player.carrying >= 0) {
            state.parcels[player.carrying].pos = next;
        }
        refreshGate(now);
        dirty = true;
    }

    void useTile(int playerId, Time now) {
        if (state.phase != Phase::Playing || now - lastActions[playerId] < actionInterval) {
            return;
        }
        lastActions[playerId] = now;
        Player& player = state.players[playerId];
        const Tile tile = tileAt(state.day, player.pos.x, player.pos.y);
        const auto end = state.parcels.begin() + orderCount(state);
        if (player.carrying == -1) {
            const auto parcel = std::find_if(state.parcels.begin(), end, [&](const Parcel& candidate) {
                return candidate.stage == OrderStage::Packed && candidate.holder == -1 && same(candidate.pos, player.pos);
            });
            if (parcel != end) {
                parcel->holder = playerId;
                player.carrying = static_cast<int>(parcel - state.parcels.begin());
                dirty = true;
            } else if (tile == Tile::Terminal) {
                const int next = firstOrder(state, OrderStage::Queued);
                if (next >= 0) {
                    state.parcels[next].stage = OrderStage::Ordered;
                    dirty = true;
                }
            }
            return;
        }

        Parcel& parcel = state.parcels[player.carrying];
        const bool delivering = tile == Tile::Destination;
        if (!delivering && std::any_of(state.parcels.begin(), end, [&](const Parcel& other) {
                return other.stage == OrderStage::Packed && other.holder == -1 && same(other.pos, player.pos);
            })) {
            return;
        }
        parcel.holder = -1;
        player.carrying = -1;
        if (delivering) {
            parcel.stage = OrderStage::Delivered;
            if (allDelivered(state)) {
                if (state.day == campaignDays) {
                    state.phase = Phase::Won;
                    notice = u8"12일간의 배달을 모두 마쳤습니다!";
                } else {
                    state.phase = state.day % 3 == 0 ? Phase::Upgrade : Phase::DayDone;
                    notice = u8"오늘 주문을 모두 배달했습니다!";
                }
            }
        }
        dirty = true;
    }

    void answerOrder(int playerId, int order, int choice, Time now) {
        Player& player = state.players[playerId];
        if (state.phase != Phase::Playing || order < 0 || order != firstOrder(state, OrderStage::Ordered)
            || player.carrying >= 0 || tileAt(state.day, player.pos.x, player.pos.y) != Tile::Packing
            || now - lastActions[playerId] < actionInterval) {
            return;
        }
        lastActions[playerId] = now;
        if (choice == campaign[state.day - 1].exercises[order].answer) {
            state.parcels[order] = {player.pos, playerId, OrderStage::Packed};
            player.carrying = order;
            for (Player& other : state.players) {
                if (other.lastWrongOrder == order) {
                    other.lastWrongOrder = -1;
                }
            }
            player.lastWrongOrder = -1;
        } else {
            player.lastWrongOrder = order;
        }
        dirty = true;
    }

    bool handlePeer(Channel& peer, const Byte* bytes, std::size_t size, Time now) {
        if (peer.id == -1) {
            if (size != 2 || bytes[0] != Hello) {
                return false;
            }
            Reason reason = static_cast<Reason>(0);
            if (bytes[1] != version) {
                reason = WrongVersion;
            } else if (playerCount(state) >= maxPlayers) {
                reason = Full;
            } else if (state.phase == Phase::Playing) {
                reason = InProgress;
            }
            if (reason != 0) {
                peer.closeAfterSend = true;
                return enqueue(peer, {Reject, reason});
            }

            for (int index = 1; index < maxPlayers; ++index) {
                if (!state.players[index].active) {
                    Point spawn = spawnFor(state.day, index);
                    for (int alternate = 0; occupied(state, spawn) && alternate < maxPlayers; ++alternate) {
                        spawn = spawnFor(state.day, alternate);
                    }
                    if (occupied(state, spawn)) {
                        return false;
                    }
                    state.players[index] = {true, spawn, -1, -1};
                    peer.id = index;
                    dirty = true;
                    return true;
                }
            }
            return false;
        }

        if (size == 4 && bytes[0] == Move) {
            const int dx = bytes[2] - 1;
            const int dy = bytes[3] - 1;
            if (!cardinal(dx, dy)) {
                return false;
            }
            if (bytes[1] == round) {
                movePlayer(peer.id, dx, dy, now);
            }
            return true;
        }
        if (size == 2 && bytes[0] == Interact) {
            if (bytes[1] == round) {
                useTile(peer.id, now);
            }
            return true;
        }
        if (size == 4 && bytes[0] == Answer && bytes[2] < parcelCount && bytes[3] < 3) {
            if (bytes[1] == round) {
                answerOrder(peer.id, bytes[2], bytes[3], now);
            }
            return true;
        }
        return size == 1 && bytes[0] == Ping;
    }

    void acceptPeers(Time now) {
        for (std::size_t attempt = 0; attempt < peers.size(); ++attempt) {
            const SOCKET socket = accept(listener, nullptr, nullptr);
            if (socket == INVALID_SOCKET) {
                return;
            }
            const auto slot = std::find_if(peers.begin(), peers.end(), [](const Channel& peer) {
                return peer.socket == INVALID_SOCKET;
            });
            if (slot == peers.end() || !nonblocking(socket)) {
                closesocket(socket);
                continue;
            }
            configureStream(socket);
            slot->socket = socket;
            slot->lastPacket = now;
        }
    }

    void pollHost(Time now) {
        refreshGate(now);
        acceptPeers(now);
        for (Channel& peer : peers) {
            if (peer.socket == INVALID_SOCKET) {
                continue;
            }
            const auto limit = peer.id == -1 ? handshakeLimit : idleLimit;
            if (now - peer.lastPacket > limit) {
                retire(peer, now);
                continue;
            }
            if (!peer.closeAfterSend && !receive(peer, now, [&](const Byte* bytes, std::size_t size) {
                    return handlePeer(peer, bytes, size, now);
                })) {
                retire(peer, now);
            }
        }

        if (dirty || now - lastSnapshot >= heartbeat) {
            dirty = false;
            lastSnapshot = now;
            for (Channel& peer : peers) {
                if (peer.id >= 0) {
                    const auto packet = encodeSnapshot(state, peer.id, round);
                    if (!enqueue(peer, packet.data(), packet.size())) {
                        retire(peer, now);
                    }
                }
            }
        }
        for (Channel& peer : peers) {
            if (peer.socket != INVALID_SOCKET
                && (!flush(peer) || (peer.closeAfterSend && peer.outgoing.empty()))) {
                retire(peer, now);
            }
        }
    }

    void pollGuest(Time now) {
        if (dialing) {
            fd_set writable;
            fd_set exception;
            FD_ZERO(&writable);
            FD_ZERO(&exception);
            FD_SET(server.socket, &writable);
            FD_SET(server.socket, &exception);
            timeval immediate{};
            const int selected = select(0, nullptr, &writable, &exception, &immediate);
            if (selected == 0 && now - server.lastPacket <= handshakeLimit) {
                return;
            }
            int error = 0;
            int length = sizeof(error);
            if (selected <= 0 || FD_ISSET(server.socket, &exception)
                || getsockopt(server.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0
                || error != 0 || !enqueue(server, {Hello, version})) {
                report(u8"호스트에 연결하지 못했습니다. IP 주소와 방 상태를 확인해 주세요.");
                return;
            }
            dialing = false;
            server.lastPacket = now;
        }

        const auto limit = id == -1 ? handshakeLimit : idleLimit;
        if (now - server.lastPacket > limit) {
            report(u8"호스트 응답 시간이 초과되었습니다.");
            return;
        }
        const char* problem = nullptr;
        if (!receive(server, now, [&](const Byte* bytes, std::size_t size) {
                if (size == 2 && bytes[0] == Reject && id == -1) {
                    problem = rejection(bytes[1]);
                    return false;
                }
                const Phase previous = state.phase;
                if (bytes[0] != Snapshot || !decodeSnapshot(bytes, size, state, id)) {
                    problem = u8"호스트에서 잘못된 게임 데이터를 받았습니다.";
                    return false;
                }
                if (round != bytes[4]) {
                    round = bytes[4];
                    lastMoves = {};
                    lastActions = {};
                }
                if (state.phase != previous) {
                    if (state.phase == Phase::Lobby) {
                        notice = previous == Phase::Connecting ? u8"방에 접속했습니다."
                            : u8"3명 미만이 되어 대기실로 돌아왔습니다.";
                    } else if (state.phase == Phase::Story) {
                        notice = u8"오늘의 배달 임무를 확인하세요.";
                    } else if (state.phase == Phase::Playing) {
                        notice = u8"배달을 시작했습니다.";
                    } else if (state.phase == Phase::DayDone || state.phase == Phase::Upgrade) {
                        notice = u8"오늘 주문을 모두 배달했습니다!";
                    } else if (state.phase == Phase::Won) {
                        notice = u8"12일간의 배달을 모두 마쳤습니다!";
                    }
                }
                return true;
            })) {
            report(problem ? problem : u8"호스트와의 연결이 끊겼습니다.");
            return;
        }
        if (id >= 0 && now - lastPing >= heartbeat) {
            lastPing = now;
            if (!enqueue(server, {Ping})) {
                report(u8"호스트 연결이 응답하지 않습니다.");
                return;
            }
        }
        if (!flush(server)) {
            report(u8"호스트와의 연결이 끊겼습니다.");
        }
    }
};

Net::Net() : impl_(new Impl) {}
Net::~Net() = default;

bool Net::host(int listenPort) {
    Impl& impl = *impl_;
    impl.reset();
    if (!impl.winsock) {
        impl.notice = u8"Windows 네트워크를 시작하지 못했습니다.";
        return false;
    }
    if (!validPort(listenPort)) {
        impl.notice = u8"포트는 1부터 65535까지 입력하세요.";
        return false;
    }
    impl.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl.listener == INVALID_SOCKET) {
        impl.report(u8"호스트 소켓을 열지 못했습니다.");
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(listenPort));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!nonblocking(impl.listener)
        || bind(impl.listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || listen(impl.listener, SOMAXCONN) != 0) {
        impl.report(u8"방을 열지 못했습니다. 선택한 포트가 사용 중인지 확인해 주세요.");
        return false;
    }
    impl.state = newState(Phase::Lobby);
    impl.boundPort = listenPort;
    impl.id = 0;
    impl.state.players[0].active = true;
    impl.dirty = true;
    impl.notice = u8"방을 열었습니다.";
    return true;
}

bool Net::join(const std::string& endpoint) {
    Impl& impl = *impl_;
    impl.reset();
    if (!impl.winsock) {
        impl.notice = u8"Windows 네트워크를 시작하지 못했습니다.";
        return false;
    }
    sockaddr_in address{};
    if (!parseAddress(endpoint, address)) {
        impl.report(u8"호스트 주소를 IPv4 또는 IPv4:포트 형식으로 입력하세요.");
        return false;
    }

    impl.server.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl.server.socket == INVALID_SOCKET || !nonblocking(impl.server.socket)) {
        impl.report(u8"접속 소켓을 열지 못했습니다.");
        return false;
    }
    configureStream(impl.server.socket);
    if (connect(impl.server.socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        enqueue(impl.server, {Hello, version});
    } else {
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEALREADY) {
            impl.report(u8"호스트에 연결하지 못했습니다.");
            return false;
        }
        impl.dialing = true;
    }
    impl.server.lastPacket = Clock::now();
    impl.boundPort = ntohs(address.sin_port);
    impl.state = newState(Phase::Connecting);
    impl.notice = u8"호스트에 연결하는 중입니다.";
    return true;
}

void Net::disconnect() {
    impl_->report(u8"연결을 종료했습니다.");
}

void Net::poll() {
    const Time now = Clock::now();
    if (isHost()) {
        impl_->pollHost(now);
    } else if (impl_->server.socket != INVALID_SOCKET) {
        impl_->pollGuest(now);
    }
}

void Net::start() {
    Impl& impl = *impl_;
    if (!isHost() || (impl.state.phase != Phase::Lobby && impl.state.phase != Phase::Won)) {
        return;
    }
    if (playerCount(impl.state) < minPlayers) {
        impl.notice = u8"배달을 시작하려면 3명 이상이 필요합니다.";
        return;
    }
    if (impl.state.phase == Phase::Won) {
        impl.resetBoard(Phase::Story, 1, false);
    } else {
        impl.state.phase = impl.resumePhase;
        impl.dirty = true;
    }
    impl.resumePhase = Phase::Story;
    impl.notice = impl.state.phase == Phase::Story ? u8"오늘의 배달 임무를 확인하세요." : u8"중단한 지점에서 이어서 진행합니다.";
}

void Net::advance() {
    Impl& impl = *impl_;
    if (!isHost()) {
        return;
    }
    if (impl.state.phase == Phase::Story) {
        ++impl.round;
        impl.state.phase = Phase::Playing;
        impl.dirty = true;
        impl.notice = u8"배달을 시작했습니다.";
    } else if (impl.state.phase == Phase::DayDone) {
        impl.resetBoard(Phase::Story, impl.state.day + 1);
        impl.notice = u8"오늘의 배달 임무를 확인하세요.";
    }
}

void Net::upgrade(int choice) {
    Impl& impl = *impl_;
    if (!isHost() || impl.state.phase != Phase::Upgrade || choice < 0 || choice > 2) {
        return;
    }
    if (choice == 0) {
        ++impl.state.speedLevel;
    } else if (choice == 1) {
        ++impl.state.bufferLevel;
    } else if (!impl.state.scanner) {
        impl.state.scanner = true;
    } else {
        impl.notice = u8"힌트 스캐너는 이미 설치했습니다. 다른 강화를 고르세요.";
        return;
    }
    impl.resetBoard(Phase::Story, impl.state.day + 1);
    impl.notice = u8"팀 장비를 강화했습니다.";
}

void Net::move(int dx, int dy) {
    Impl& impl = *impl_;
    if (impl.id == -1 || impl.state.phase != Phase::Playing || !cardinal(dx, dy)) {
        return;
    }
    const Time now = Clock::now();
    if (isHost()) {
        impl.refreshGate(now);
        impl.movePlayer(impl.id, dx, dy, now);
    } else if (now - impl.lastMoves[impl.id] >= moveInterval(impl.state)) {
        impl.lastMoves[impl.id] = now;
        if (!enqueue(impl.server, {Move, impl.round, static_cast<Byte>(dx + 1), static_cast<Byte>(dy + 1)})) {
            impl.report(u8"호스트 연결이 응답하지 않습니다.");
        }
    }
}

void Net::answer(int choice) {
    Impl& impl = *impl_;
    if (impl.id == -1 || impl.state.phase != Phase::Playing || choice < 0 || choice > 2) {
        return;
    }
    const Player& player = impl.state.players[impl.id];
    const int order = firstOrder(impl.state, OrderStage::Ordered);
    if (order < 0 || player.carrying >= 0 || tileAt(impl.state.day, player.pos.x, player.pos.y) != Tile::Packing) {
        return;
    }
    const Time now = Clock::now();
    if (isHost()) {
        impl.answerOrder(impl.id, order, choice, now);
    } else if (now - impl.lastActions[impl.id] >= actionInterval) {
        impl.lastActions[impl.id] = now;
        if (!enqueue(impl.server, {Answer, impl.round, static_cast<Byte>(order), static_cast<Byte>(choice)})) {
            impl.report(u8"호스트 연결이 응답하지 않습니다.");
        }
    }
}

void Net::interact() {
    Impl& impl = *impl_;
    if (impl.id == -1 || impl.state.phase != Phase::Playing) {
        return;
    }
    const Time now = Clock::now();
    if (isHost()) {
        impl.useTile(impl.id, now);
    } else if (now - impl.lastActions[impl.id] >= actionInterval) {
        impl.lastActions[impl.id] = now;
        if (!enqueue(impl.server, {Interact, impl.round})) {
            impl.report(u8"호스트 연결이 응답하지 않습니다.");
        }
    }
}

const State& Net::state() const { return impl_->state; }
int Net::myId() const { return impl_->id; }
bool Net::isHost() const { return impl_->listener != INVALID_SOCKET; }
int Net::roomPort() const { return impl_->boundPort; }
const std::string& Net::notice() const { return impl_->notice; }

std::string Net::localAddress() const {
    const char* fallback = "127.0.0.1";
    if (!impl_->winsock) {
        return fallback;
    }
    const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET) {
        return fallback;
    }
    sockaddr_in route{};
    route.sin_family = AF_INET;
    route.sin_port = htons(port);
    InetPtonA(AF_INET, "192.0.2.1", &route.sin_addr);
    sockaddr_in local{};
    int length = sizeof(local);
    char text[INET_ADDRSTRLEN]{};
    if (connect(probe, reinterpret_cast<const sockaddr*>(&route), sizeof(route)) == 0
        && getsockname(probe, reinterpret_cast<sockaddr*>(&local), &length) == 0
        && InetNtopA(AF_INET, &local.sin_addr, text, sizeof(text))) {
        closesocket(probe);
        return text;
    }
    std::array<INTERFACE_INFO, 32> interfaces{};
    DWORD written = 0;
    if (WSAIoctl(probe, SIO_GET_INTERFACE_LIST, nullptr, 0, interfaces.data(),
                 static_cast<DWORD>(sizeof(interfaces)), &written, nullptr, nullptr) == 0) {
        const auto end = interfaces.begin() + written / sizeof(INTERFACE_INFO);
        const auto first = std::find_if(interfaces.begin(), end, [](const INTERFACE_INFO& info) {
            return (info.iiFlags & IFF_UP) && !(info.iiFlags & IFF_LOOPBACK);
        });
        if (first != end && InetNtopA(AF_INET, &first->iiAddress.AddressIn.sin_addr, text, sizeof(text))) {
            closesocket(probe);
            return text;
        }
    }
    closesocket(probe);
    return fallback;
}

}

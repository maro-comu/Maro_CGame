#ifndef NETWORK_MARO_H
#define NETWORK_MARO_H

#include <array>
#include <memory>
#include <string>

namespace maro {

constexpr int width = 18;
constexpr int height = 11;
constexpr int minPlayers = 3;
constexpr int maxPlayers = 6;
constexpr int parcelCount = 3;
constexpr int port = 47640;

enum class Phase { Disconnected, Connecting, Lobby, Story, Playing, DayDone, Upgrade, Won };
enum class Tile { Floor, Wall, Gate, Switch, Destination, Terminal, Packing };
enum class OrderStage { Queued, Ordered, Packed, Delivered };

struct Point {
    int x;
    int y;
};

struct Player {
    bool active;
    Point pos;
    int carrying;
    int lastWrongOrder;
};

struct Parcel {
    Point pos;
    int holder;
    OrderStage stage;
};

struct State {
    Phase phase;
    bool gateOpen;
    int day;
    int speedLevel;
    int bufferLevel;
    bool scanner;
    std::array<Player, maxPlayers> players;
    std::array<Parcel, parcelCount> parcels;
};

Tile tileAt(int day, int x, int y);

class Net {
public:
    Net();
    ~Net();
    Net(const Net&) = delete;
    Net& operator=(const Net&) = delete;

    bool host(int listenPort = port);
    bool join(const std::string& address);
    void disconnect();
    void poll();
    void start();
    void advance();
    void upgrade(int choice);
    void move(int dx, int dy);
    void interact();
    void answer(int choice);

    const State& state() const;
    int myId() const;
    bool isHost() const;
    int roomPort() const;
    const std::string& notice() const;
    std::string localAddress() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif

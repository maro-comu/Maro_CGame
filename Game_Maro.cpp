#include "Campaign_Maro.h"
#include "Network_Maro.h"

#include "cocos2d.h"
#include "ui/CocosGUI.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string>

namespace {

using namespace cocos2d;
using namespace maro;

constexpr float canvasWidth = 1280;
constexpr float canvasHeight = 800;
constexpr float cell = 44;
constexpr float boardLeft = 32;
constexpr float boardBottom = 147;
constexpr float rail = 856;
const Color3B green(126, 247, 157);
const Color3B pale(223, 240, 227);
const Color3B dim(143, 174, 153);
const Color3B gold(255, 200, 112);
const std::array<Color3B, maxPlayers> colors{{
    {82, 223, 255}, {255, 183, 94}, {218, 160, 255},
    {255, 140, 137}, {183, 227, 113}, {135, 178, 255},
}};

std::string asset(const std::string& name)
{
    return "Assets_Maro/" + name + "_Maro.png";
}

std::string fontPath(const char* file)
{
    static const std::string directory = [] {
        char windows[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryA(windows, MAX_PATH);
        return std::string(length && length < MAX_PATH ? windows : "C:/Windows") + "/Fonts/";
    }();
    return directory + file;
}

Label* label(const std::string& text, float size, Color3B color, bool monospace = false)
{
    auto* result = Label::createWithTTF(text.empty() ? " " : text,
        fontPath(monospace ? "consola.ttf" : "malgun.ttf"), size);
    if (!result) {
        result = Label::createWithSystemFont(text, monospace ? "Consolas" : "Malgun Gothic", size);
    }
    result->setColor(color);
    return result;
}

Label* text(Node* parent, const std::string& value, float size, Vec2 position,
            Color3B color = pale, bool monospace = false)
{
    auto* result = label(value, size, color, monospace);
    result->setAnchorPoint({0, 0.5f});
    result->setPosition(position);
    parent->addChild(result);
    return result;
}

MenuItemLabel* command(Node* parent, const std::string& title, Vec2 position,
                       const std::function<void()>& action, Color3B color = green, float size = 23)
{
    auto* item = MenuItemLabel::create(label(title, size, color), [action](Ref*) { action(); });
    item->setAnchorPoint({0, 0.5f});
    item->setDisabledColor({95, 114, 100});
    auto* menu = Menu::create(item, nullptr);
    menu->setPosition(position);
    parent->addChild(menu);
    return item;
}

void shade(Node* parent, Vec2 from, Size size)
{
    auto* node = DrawNode::create();
    node->drawSolidRect(from, from + Vec2(size.width, size.height), {0.023f, 0.057f, 0.041f, 0.90f});
    parent->addChild(node);
}

Sprite* sprite(Node* parent, const std::string& name, Vec2 center, Size size)
{
    auto* result = Sprite::create(asset(name));
    result->setPosition(center);
    result->setScaleX(size.width / result->getContentSize().width);
    result->setScaleY(size.height / result->getContentSize().height);
    parent->addChild(result);
    return result;
}

Vec2 centerOf(maro::Point point)
{
    return {boardLeft + (point.x + 0.5f) * cell, boardBottom + (height - point.y - 0.5f) * cell};
}

const char* tileArt(maro::Tile tile)
{
    switch (tile) {
    case maro::Tile::Wall: return "wall";
    case maro::Tile::Gate: return "gate_closed";
    case maro::Tile::Switch: return "switch_off";
    case maro::Tile::Destination: return "destination";
    case maro::Tile::Terminal: return "terminal";
    case maro::Tile::Packing: return "packing";
    case maro::Tile::Floor: return "floor";
    }
    return "floor";
}

std::string completedCode(const Exercise& exercise)
{
    std::string code = exercise.code;
    const auto slot = code.find("___");
    if (slot != std::string::npos) {
        code.replace(slot, 3, exercise.choices[exercise.answer]);
    }
    return code;
}

class CourierScene final : public Scene {
public:
    CREATE_FUNC(CourierScene);

    bool init() override
    {
        if (!Scene::init()) {
            return false;
        }
        window_ = static_cast<GLViewImpl*>(Director::getInstance()->getOpenGLView())->getWindow();
        glfwSetInputMode(window_, GLFW_STICKY_KEYS, GLFW_TRUE);
        sprite(this, "background", {640, 400}, {canvasWidth, canvasHeight});
        text(this, "C:\\CMD>", 24, {32, 762}, green, true);
        command(this, "-", {1172, 762}, [this] { glfwIconifyWindow(window_); }, dim, 25);
        command(this, "×", {1222, 762}, [this] { glfwSetWindowShouldClose(window_, GLFW_TRUE); }, pale, 28);
        buildLobby();
        buildDepot();
        buildInterlude();
        refresh();
        scheduleUpdate();
        return true;
    }

    void update(float delta) override
    {
        net_.poll();
        controls(delta);
        refresh();
    }

private:
    void buildLobby()
    {
        lobby_ = Node::create();
        addChild(lobby_);
        text(lobby_, "코드 배달부", 65, {86, 567});
        text(lobby_, "주문 → 포장 → 배달", 28, {91, 481}, green);
        text(lobby_, "3–6인  /  12일", 19, {93, 428}, dim);

        offline_ = Node::create();
        lobby_->addChild(offline_);
        command(offline_, "[ 방 만들기 ]", {89, 332}, [this] {
            if (net_.host()) {
                room_ = net_.localAddress() + ":" + std::to_string(net_.roomPort());
            }
        });
        text(offline_, "방장 IP 또는 IP:포트", 18, {90, 253}, dim);
        shade(offline_, {88, 174}, {312, 53});
        address_ = ui::EditBox::create({297, 48}, ui::Scale9Sprite::create());
        address_->setPosition({244, 200});
        address_->setFont("Consolas", 23);
        address_->setFontColor(pale);
        address_->setMaxLength(21);
        address_->setInputMode(ui::EditBox::InputMode::SINGLE_LINE);
        address_->setText("127.0.0.1");
        offline_->addChild(address_);
        command(offline_, "[ 참가 ]", {423, 199}, [this] { net_.join(address_->getText()); });

        online_ = Node::create();
        lobby_->addChild(online_);
        roomLabel_ = text(online_, " ", 22, {91, 327}, green);
        start_ = command(online_, "[ 시작 ]", {89, 232}, [this] { net_.start(); });
        command(online_, "[ 나가기 ]", {246, 232}, [this] { net_.disconnect(); }, dim);
        count_ = text(online_, " ", 25, {824, 550}, green);
        for (int i = 0; i < maxPlayers; ++i) {
            const float y = 491.0f - i * 50;
            lobbyRobots_[i] = sprite(online_, "robot" + std::to_string(i + 1), {846, y}, {43, 43});
            names_[i] = text(online_, " ", 19, {879, y}, colors[i]);
        }
        notice_ = text(lobby_, " ", 19, {89, 111}, gold);
    }

    void buildDepot()
    {
        depot_ = Node::create();
        addChild(depot_);
        day_ = text(depot_, " ", 31, {32, 706}, pale);
        progress_ = text(depot_, " ", 22, {rail, 707}, green);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                tiles_[y * width + x] = sprite(depot_, "floor", centerOf({x, y}), {cell, cell});
            }
        }
        for (auto& parcel : parcels_) {
            parcel = sprite(depot_, "package", Vec2::ZERO, {36, 36});
        }
        for (int i = 0; i < maxPlayers; ++i) {
            actors_[i] = Node::create();
            depot_->addChild(actors_[i]);
            auto* halo = DrawNode::create();
            halo->drawSolidCircle({0, -10}, 17, 0, 24, Color4F(colors[i]));
            halo->drawSolidCircle({0, -10}, 14, 0, 24, {0.02f, 0.06f, 0.04f, 1});
            actors_[i]->addChild(halo);
            halos_[i] = halo;
            sprite(actors_[i], "robot" + std::to_string(i + 1), {0, 2}, {41, 41});
            cargo_[i] = sprite(actors_[i], "package", {13, 12}, {23, 23});
        }

        gateStatus_ = text(depot_, " ", 18, {rail, 638}, dim);
        task_ = text(depot_, " ", 21, {rail, 591}, gold);
        code_ = text(depot_, " ", 17, {rail, 553}, green, true);
        code_->setAnchorPoint({0, 1});
        goal_ = text(depot_, " ", 16, {rail, 411}, pale);
        goal_->setDimensions(382, 0);
        goal_->setAnchorPoint({0, 1});
        for (int i = 0; i < 3; ++i) {
            answers_[i] = command(depot_, " ", {rail, 347.0f - i * 41},
                [this, i] { net_.answer(i); }, pale, 19);
        }
        detail_ = text(depot_, " ", 17, {rail, 222}, dim);
        detail_->setDimensions(382, 0);
        detail_->setAnchorPoint({0, 1});
        objective_ = text(depot_, " ", 20, {32, 101}, green);
        text(depot_, "이동  WASD / 방향키     작업  SPACE     포장 선택  1–3", 16, {32, 54}, dim);
        command(depot_, "[ 나가기 ]", {1111, 55}, [this] { net_.disconnect(); }, dim, 19);
    }

    void buildInterlude()
    {
        interlude_ = Node::create();
        addChild(interlude_);
        chapter_ = text(interlude_, " ", 23, {107, 629}, green);
        heading_ = text(interlude_, " ", 48, {105, 547});
        story_ = text(interlude_, " ", 27, {108, 467}, pale);
        story_->setDimensions(1030, 0);
        story_->setAnchorPoint({0, 1});
        next_ = command(interlude_, "[ 계속 ]", {105, 178}, [this] {
            if (net_.state().phase == Phase::Won) {
                net_.start();
            } else {
                net_.advance();
            }
        });
        wait_ = text(interlude_, "방장이 진행할 때까지 기다리는 중", 19, {107, 178}, dim);
        command(interlude_, "[ 나가기 ]", {1111, 55}, [this] { net_.disconnect(); }, dim, 19);

        const std::array<const char*, 3> upgrades{{
            "[1] 가벼운 바퀴   ·   이동 속도 증가",
            "[2] 예비 전원   ·   스위치에서 내려와도 잠시 통로 유지",
            "[3] 코드 스캐너   ·   포장 전에 힌트 표시",
        }};
        for (int i = 0; i < 3; ++i) {
            upgrade_[i] = command(interlude_, upgrades[i], {106, 336.0f - i * 62},
                [this, i] { net_.upgrade(i); });
        }
    }

    void controls(float delta)
    {
        const auto phase = net_.state().phase;
        const bool playing = phase == Phase::Playing;
        const bool space = glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (playing && space && !space_) {
            net_.interact();
        }
        space_ = space;
        for (int i = 0; i < 3; ++i) {
            const bool digit = glfwGetKey(window_, GLFW_KEY_1 + i) == GLFW_PRESS;
            if (digit && !digits_[i]) {
                if (playing) {
                    net_.answer(i);
                } else if (phase == Phase::Upgrade && net_.isHost()) {
                    net_.upgrade(i);
                }
            }
            digits_[i] = digit;
        }

        moveDelay_ -= delta;
        if (playing && moveDelay_ > 0) {
            return;
        }
        const struct { int letter; int arrow; int dx; int dy; } moves[] = {
            {GLFW_KEY_W, GLFW_KEY_UP, 0, -1}, {GLFW_KEY_S, GLFW_KEY_DOWN, 0, 1},
            {GLFW_KEY_A, GLFW_KEY_LEFT, -1, 0}, {GLFW_KEY_D, GLFW_KEY_RIGHT, 1, 0},
        };
        for (const auto& move : moves) {
            const bool letter = glfwGetKey(window_, move.letter) == GLFW_PRESS;
            const bool arrow = glfwGetKey(window_, move.arrow) == GLFW_PRESS;
            if (playing && (letter || arrow)) {
                net_.move(move.dx, move.dy);
                moveDelay_ = 0.13f - 0.018f * net_.state().speedLevel;
                break;
            }
        }
    }

    void refresh()
    {
        const State& state = net_.state();
        const bool inLobby = state.phase == Phase::Disconnected || state.phase == Phase::Connecting || state.phase == Phase::Lobby;
        const bool playing = state.phase == Phase::Playing;
        lobby_->setVisible(inLobby);
        depot_->setVisible(playing);
        interlude_->setVisible(!inLobby && !playing);
        address_->setVisible(state.phase == Phase::Disconnected);
        if (inLobby) {
            refreshLobby(state);
        } else if (playing) {
            refreshDepot(state);
        } else {
            refreshInterlude(state);
        }
    }

    void refreshLobby(const State& state)
    {
        const bool offline = state.phase == Phase::Disconnected;
        offline_->setVisible(offline);
        online_->setVisible(!offline);
        notice_->setString(net_.notice());
        if (offline) {
            return;
        }
        const bool host = net_.isHost();
        const auto active = std::count_if(state.players.begin(), state.players.end(),
            [](const Player& player) { return player.active; });
        count_->setString("인원  " + std::to_string(active) + " / " + std::to_string(maxPlayers));
        start_->setVisible(host);
        start_->setEnabled(active >= minPlayers);
        roomLabel_->setString(host ? room_ : state.phase == Phase::Connecting ? "연결 중…" : "방장 대기 중");
        for (int i = 0; i < maxPlayers; ++i) {
            const bool present = state.players[i].active;
            lobbyRobots_[i]->setVisible(present);
            names_[i]->setString(present ? "로봇 " + std::to_string(i + 1) + (i == net_.myId() ? " (나)" : "") : "");
        }
    }

    void refreshMap(const State& state)
    {
        const bool newDay = drawnDay_ != state.day;
        if (newDay) {
            switches_.fill(-1);
            gateTile_ = -1;
            int switchIndex = 0;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const auto tile = tileAt(state.day, x, y);
                    const int index = y * width + x;
                    tiles_[index]->setTexture(asset(tileArt(tile)));
                    if (tile == maro::Tile::Gate) {
                        gateTile_ = index;
                    } else if (tile == maro::Tile::Switch) {
                        switches_[switchIndex++] = index;
                    }
                }
            }
            drawnDay_ = state.day;
        }
        if (gateTile_ >= 0 && (newDay || gateOpen_ != state.gateOpen)) {
            tiles_[gateTile_]->setTexture(asset(state.gateOpen ? "gate_open" : "gate_closed"));
        }
        gateOpen_ = state.gateOpen;
        int occupied = 0;
        for (size_t i = 0; i < switches_.size(); ++i) {
            const int index = switches_[i];
            const bool pressed = index >= 0 && std::any_of(state.players.begin(), state.players.end(), [index](const Player& player) {
                return player.active && player.pos.x == index % width && player.pos.y == index / width;
            });
            if (index >= 0 && (newDay || switchPressed_[i] != pressed)) {
                tiles_[index]->setTexture(asset(pressed ? "switch_on" : "switch_off"));
            }
            switchPressed_[i] = pressed;
            occupied += pressed ? 1 : 0;
        }
        gateStatus_->setString(std::string(state.gateOpen ? "통로 열림" : "통로 잠김") + "  ·  스위치 " + std::to_string(occupied) + "/2");
        gateStatus_->setColor(state.gateOpen ? green : dim);
    }

    void refreshDepot(const State& state)
    {
        const Day& day = campaign[state.day - 1];
        day_->setString(std::to_string(state.day) + "일차  /  " + day.title);
        refreshMap(state);
        int ordered = 0;
        int packed = 0;
        int delivered = 0;
        int nextOrder = -1;
        for (int i = 0; i < parcelCount; ++i) {
            const auto& parcel = state.parcels[i];
            parcels_[i]->setVisible(i < day.orderCount && parcel.stage == OrderStage::Packed && parcel.holder < 0);
            parcels_[i]->setPosition(centerOf(parcel.pos));
            if (i >= day.orderCount) {
                continue;
            }
            ordered += parcel.stage != OrderStage::Queued;
            packed += parcel.stage == OrderStage::Packed || parcel.stage == OrderStage::Delivered;
            delivered += parcel.stage == OrderStage::Delivered;
            if (nextOrder < 0 && parcel.stage == OrderStage::Ordered) {
                nextOrder = i;
            }
        }
        const std::string total = "/" + std::to_string(day.orderCount);
        progress_->setString("주문 " + std::to_string(ordered) + total + "   포장 " + std::to_string(packed) +
            total + "   배달 " + std::to_string(delivered) + total);

        for (int i = 0; i < maxPlayers; ++i) {
            const Player& player = state.players[i];
            actors_[i]->setVisible(player.active);
            actors_[i]->setPosition(centerOf(player.pos));
            halos_[i]->setVisible(i == net_.myId());
            cargo_[i]->setVisible(player.carrying >= 0);
        }
        const int me = net_.myId();
        if (me >= 0 && me < maxPlayers) {
            refreshTask(state, state.players[me], nextOrder, ordered);
        }
    }

    void refreshTask(const State& state, const Player& player, int nextOrder, int ordered)
    {
        const Day& day = campaign[state.day - 1];
        const bool carrying = player.carrying >= 0;
        const bool pending = nextOrder >= 0;
        const auto tile = tileAt(state.day, player.pos.x, player.pos.y);
        const bool canPack = pending && !carrying && tile == maro::Tile::Packing;
        const Exercise* exercise = carrying ? &day.exercises[player.carrying] : pending ? &day.exercises[nextOrder] : nullptr;

        task_->setString(carrying ? "포장 완료" : pending ? "포장  /  " + std::string(day.concept) : day.concept);
        code_->setString(exercise ? carrying ? completedCode(*exercise) : exercise->code : "");
        goal_->setString(pending && !carrying ? exercise->goal : "");
        for (int i = 0; i < 3; ++i) {
            answers_[i]->setVisible(canPack);
            if (canPack) {
                answers_[i]->setString("[" + std::to_string(i + 1) + "]  " + day.exercises[nextOrder].choices[i]);
            }
        }
        const bool wrong = pending && player.lastWrongOrder == nextOrder;
        detail_->setString(carrying ? exercise->explanation : pending && (state.scanner || wrong) ? exercise->hint : "");
        detail_->setColor(wrong && !carrying ? gold : dim);

        if (carrying) {
            if (tile == maro::Tile::Destination) {
                objective_->setString("SPACE  ·  화물 인계");
            } else {
                objective_->setString(state.gateOpen ? "오른쪽 목적지까지 운반하세요."
                    : "통로가 막혔다면 두 사람이 각각 스위치를 밟아 주세요.");
            }
        } else if (canPack) {
            objective_->setString(wrong ? "코드를 다시 확인하세요.  ·  1–3" : "1–3  ·  빈칸에 들어갈 코드로 포장");
        } else if (tile == maro::Tile::Terminal && ordered < day.orderCount) {
            objective_->setString("SPACE  ·  주문 받기");
        } else if (tile == maro::Tile::Switch) {
            objective_->setString("스위치를 밟고 있는 동안 팀의 통로를 유지합니다.");
        } else if (pending) {
            objective_->setString("노란 포장대에서 주문 코드의 빈칸을 채우세요.");
        } else if (ordered < day.orderCount) {
            objective_->setString("초록 단말에서 SPACE를 눌러 주문을 받으세요.");
        } else {
            objective_->setString("남은 화물을 옮기거나 스위치를 지켜 주세요.");
        }
    }

    void refreshInterlude(const State& state)
    {
        const Day& day = campaign[state.day - 1];
        const bool host = net_.isHost();
        const bool selecting = state.phase == Phase::Upgrade;
        const bool won = state.phase == Phase::Won;
        chapter_->setString(std::to_string(state.day) + " / " + std::to_string(campaignDays) + "일차");
        next_->setVisible(host && !selecting);
        wait_->setVisible(!host);
        next_->setString(won ? "[ 처음부터 ]" : state.phase == Phase::Story ? "[ 배송 시작 ]" : "[ 다음 날 ]");
        for (auto* choice : upgrade_) {
            choice->setVisible(selecting);
            choice->setEnabled(host);
        }
        upgrade_[2]->setEnabled(host && !state.scanner);

        if (state.phase == Phase::Story) {
            heading_->setString(day.title);
            story_->setString(day.story);
        } else if (selecting) {
            heading_->setString("팀 증강 선택");
            story_->setString("다음 근무부터 팀 전체에 적용할 장비를 고르세요.");
        } else if (won) {
            heading_->setString("12일의 배달 완료");
            story_->setString("첫 main 함수에서 마지막 코드까지, 팀이 모든 주문을 전달했습니다.");
        } else {
            heading_->setString("오늘의 배달 완료");
            story_->setString(day.exercises[day.orderCount - 1].explanation);
        }
    }

    Net net_;
    GLFWwindow* window_{};
    Node *lobby_{}, *offline_{}, *online_{}, *depot_{}, *interlude_{};
    ui::EditBox* address_{};
    MenuItemLabel *start_{}, *next_{};
    Label *roomLabel_{}, *count_{}, *notice_{}, *day_{}, *progress_{}, *gateStatus_{},
          *task_{}, *code_{}, *goal_{}, *detail_{}, *objective_{}, *chapter_{}, *heading_{}, *story_{}, *wait_{};
    std::array<MenuItemLabel*, 3> answers_{}, upgrade_{};
    std::array<Sprite*, width * height> tiles_{};
    std::array<Sprite*, parcelCount> parcels_{};
    std::array<Sprite*, maxPlayers> lobbyRobots_{}, cargo_{};
    std::array<Label*, maxPlayers> names_{};
    std::array<Node*, maxPlayers> actors_{}, halos_{};
    std::array<int, 2> switches_{};
    std::array<bool, 2> switchPressed_{};
    std::array<bool, 3> digits_{};
    int drawnDay_{};
    int gateTile_{-1};
    bool gateOpen_{};
    bool space_{};
    float moveDelay_{};
    std::string room_;
};

class CourierApp final : public Application {
public:
    bool applicationDidFinishLaunching() override
    {
        const char* required[] = {"background", "floor", "wall", "gate_open", "gate_closed", "switch_on",
            "switch_off", "destination", "terminal", "packing", "package", "robot1", "robot2", "robot3",
            "robot4", "robot5", "robot6"};
        for (const auto* name : required) {
            if (!FileUtils::getInstance()->isFileExist(asset(name))) {
                cocos2d::ccMessageBox("Resources/Assets_Maro is incomplete.", "Delivery_Maro");
                return false;
            }
        }

        RECT desktop{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
        const float fit = std::min({1.0f, (desktop.right - desktop.left - 40) / canvasWidth,
            (desktop.bottom - desktop.top - 55) / canvasHeight});
        auto* view = GLViewImpl::createWithRect("CMD Code Courier", {0, 0, canvasWidth, canvasHeight},
            fit > 0.3f ? fit : 1.0f, false);
        if (!view) {
            return false;
        }
        HWND handle = glfwGetWin32Window(view->getWindow());
        RECT client{};
        GetClientRect(handle, &client);
        POINT origin{};
        ClientToScreen(handle, &origin);
        const auto style = GetWindowLongPtrW(handle, GWL_STYLE);
        SetWindowLongPtrW(handle, GWL_STYLE, style & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowPos(handle, nullptr, origin.x, origin.y, client.right, client.bottom,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

        auto* director = Director::getInstance();
        director->setOpenGLView(view);
        view->setDesignResolutionSize(canvasWidth, canvasHeight, ResolutionPolicy::SHOW_ALL);
        director->setAnimationInterval(1.0 / 60.0);
        director->setDisplayStats(false);
        director->runWithScene(CourierScene::create());
        return true;
    }

    void applicationDidEnterBackground() override {}
    void applicationWillEnterForeground() override {}
};

}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    CourierApp app;
    return Application::getInstance()->run();
}

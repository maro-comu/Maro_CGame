# CMD 코드 배달부

3~6명이 한 팀으로 주문을 받고, C 코드로 포장하고, 화물을 배달하는 Windows 게임입니다. 12일 동안 맵과 주문이 바뀝니다.

## 실행과 접속

1. [v0.1 실행 ZIP](https://github.com/maro-comu/Maro_CGame/releases/download/v0.1/Delivery_Maro_Win32.zip)을 풀고 `Delivery_Maro.exe`를 엽니다. DLL과 `Resources` 폴더는 실행 파일 옆에 둡니다.
2. 한 사람이 **방 만들기**를 누르고, 나머지는 주소를 입력해 **참가**합니다. 방장을 포함해 3명부터 시작할 수 있습니다.
3. 같은 네트워크에서는 방 화면에 표시된 IPv4 주소를 입력합니다. 같은 PC에서 확인할 때는 게임을 세 번 열고 나머지 두 창에서 `127.0.0.1`로 참가합니다.

다른 네트워크에서 접속할 때는 참가자가 방장의 **공인 IPv4 주소**를 입력합니다. 방장이 공유기 뒤에 있다면 공유기의 외부 TCP 포트를 게임 PC의 TCP `47640`으로 전달해야 합니다. 외부 포트가 `47640`과 다르면 참가자가 `IPv4:외부포트` 형식으로 입력합니다. Windows 방화벽이 수신 연결을 차단하는 환경에서는 방장의 네트워크 설정도 필요합니다. 공인 IPv4 수신이 불가능한 공유망에서는 직접 연결할 수 없습니다.

`MSVCP140.dll` 또는 `VCRUNTIME140.dll`이 없다는 메시지가 나오면 [Microsoft Visual C++ 재배포 패키지 x86](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist#latest-supported-redistributable-version)이 필요합니다.

## 한 날의 배달

1. **주문.** 초록색 단말 위에서 **Space**를 누릅니다.
2. **포장.** 노란색 포장대로 이동하고 주문 목표에 맞는 코드 조각을 **1–3** 또는 클릭으로 고릅니다. 맞히면 완성된 코드와 해설이 표시됩니다. 다시 풀 때는 힌트를 볼 수 있습니다.
3. **배달.** 다른 두 로봇이 스위치를 밟아 통로를 열어 주면, 화물을 들고 목적지로 이동해 **Space**로 인계합니다. 떨어진 화물도 그 칸에서 **Space**로 다시 들 수 있습니다.

이동에는 **WASD**나 **방향키**를 사용합니다. 하루의 주문을 모두 배달하면 다음 날로 넘어갑니다. 3일, 6일, 9일 종료 후 방장이 팀과 상의해 이동 속도, 통로 유지 시간, 코드 스캐너 중 하나를 고릅니다. 새 근무일에는 짧은 이야기가 나오고 12일차를 마치면 캠페인이 끝납니다. 캠페인 상태는 방이 열려 있는 동안 유지됩니다.

## 소스와 에셋

- `Game_Maro.cpp`는 Cocos2d-x 화면과 조작을 담당합니다.
- `Network_Maro.cpp`, `Network_Maro.h`는 접속, 플레이어 동기화, 주문·포장·배달과 증강 판정을 담당합니다.
- `Campaign_Maro.h`에는 맵 12개, 이야기, C 코드 주문서가 있습니다.
- `Assets_Maro/`에는 별도 PNG 에셋이 들어 있습니다.

## 소스 빌드

Win32용 MSVC와 외부 의존성까지 준비된 [Cocos2d-x 4.0 SDK](https://github.com/cocos2d/cocos2d-x/tree/cocos2d-x-4.0)가 필요합니다. Git 소스에서 시작한다면 [고정 의존성 아카이브](https://github.com/cocos2d/cocos2d-x-3rd-party-libs-bin/archive/metal-support-22.zip)의 내부 파일을 SDK의 `external` 폴더에 넣습니다.

Visual Studio 2026 개발자 PowerShell에서 `Maro_CGame` 소스 폴더를 열고 빌드합니다. `$sdk`는 실제 SDK 경로로 바꿉니다.

```powershell
$sdk = 'C:\SDK\cocos2d-x-4.0'
cmake -S . -B build -G 'Visual Studio 18 2026' -A Win32 "-DCOCOS2DX_ROOT_PATH=$sdk" '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
cmake --build build --config Release --target Delivery_Maro
.\build\bin\Delivery_Maro\Release\Delivery_Maro.exe
```

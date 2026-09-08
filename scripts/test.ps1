$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force build | Out-Null
    & g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude src/people_counter.cpp tests/counter_tests.cpp -o build/counter_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed' }
    & ./build/counter_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    & g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude src/people_counter.cpp examples/simulate.cpp -o build/counter_sim.exe
    if ($LASTEXITCODE -ne 0) { throw 'Simulation compilation failed' }
    & ./build/counter_sim.exe
    if ($LASTEXITCODE -ne 0) { throw 'Simulation failed' }
    & g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude src/people_counter.cpp src/room_state.cpp tests/room_state_tests.cpp -o build/room_state_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'RoomState test compilation failed' }
    & ./build/room_state_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'RoomState tests failed' }
    & g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude src/people_counter.cpp src/room_state.cpp apps/people_counter_gateway.cpp -o build/people_counter_gateway.exe
    if ($LASTEXITCODE -ne 0) { throw 'Gateway compilation failed' }
    & python tests/test_desktop.py build/people_counter_gateway.exe
    if ($LASTEXITCODE -ne 0) { throw 'Desktop integration tests failed' }
} finally { Pop-Location }

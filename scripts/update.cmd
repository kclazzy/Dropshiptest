@echo off
rem Обновление в Windows: git pull + пересборка. Данные пользователя не затрагиваются.
cd /d "%~dp0\.."
git pull --ff-only || exit /b 1
cmake -S core-cpp -B core-cpp\build -DDROPSHIP_BUILD_TESTS=OFF || exit /b 1
cmake --build core-cpp\build --config Release --target dropship_core_app || exit /b 1
pushd extension-mv3
call npm ci --no-audit --no-fund && call npm run build
popd

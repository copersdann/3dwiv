# 3Dwiv — просмотрщик 3D-CAD форматов для Android (низкоуровневый, max скорость)

[![Android](https://img.shields.io/badge/platform-Android-3DDC84?logo=android&logoColor=white)](https://developer.android.com/)
[![C++17](https://img.shields.io/badge/native-C%2B%2B17-00599C?logo=c%2B%2B&logoColor=white)](https://isocpp.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Приложение для просмотра 3D-моделей: **STL, 3MF, STEP, OBJ, PLY, glTF/GLB, AMF**.
Вся тяжёлая работа выполняется в **C++ через NDK**, рендеринг — **OpenGL ES 3.0**.
Kotlin — UI-обвязка (адаптивный тёмный интерфейс, жесты, настройки).

## Интерфейс и функции (этап 3)

- 🎨 **Адаптивный тёмный UI** на чистом Android-фреймворке: edge-to-edge
  (insets статус-бара/нав-бара), ripple-кнопки, градиентный фон, стат-чип,
  плавающая панель масштаба, нижняя панель действий, bottom-sheet настроек.
- ⚙️ **Настройки (SharedPreferences, сохраняются)**:
  - Фон: Тёмный / Светлый / Графит
  - Свет (рассеянный ambient, 0–100%)
  - Угол обзора FOV (15–110°)
  - Оси координат XYZ (RGB-линии, depth-тест)
  - Сглаживание **MSAA: выкл / ×2 / ×4** (пересоздание EGL-конфигурации)
  - Чувствительность вращения (40–180%)
  - Автоподгонка камеры при загрузке
- 🔍 Управление: вращение (1 палец), масштаб+панорама (2 пальца),
  **двойной тап — быстрое приближение**, кнопки +/− масштаба, «Подогнать».
- 📸 **Скриншот** сцены (PixelCopy → галерея Pictures/3Dwiv).
- 📏 Статистика: имя файла, треугольники, вершины, **размер модели** (span).

## Поддерживаемые форматы

| Формат | Расширения | Что умеем |
|---|---|---|
| STL | `.stl` | binary (осн.), ASCII; автоопределение |
| 3MF | `.3mf` | собственный DEFLATE (RFC 1951), ZIP, XML-скан, `<build>`-трансформы |
| STEP | `.step`, `.stp` | BREP: plane/cylinder/cone/sphere/torus, trim line/circle/ellipse, отверстия (мосты + ear-clipping) |
| OBJ | `.obj` | `v`/`f` (все 4 формы индексов, отрицательные, n-угольники веером) |
| PLY | `.ply` | ascii + binary (little/big endian), любые свойства, list-face |
| glTF | `.glb`, `.gltf` | glTF 2.0: буферы/bufferView/accessors, node-иерархия (matrix/TRS), data URI, modes TRI/STRIP/FAN |
| AMF | `.amf` | XML: vertices/volume/triangle, все объекты |

Все парсеры — собственная реализация на C++: **ноль внешних библиотек**.

## Сборка

Требуется **Android Studio** (Koala+):

1. `File → Open` → папка проекта
2. Согласиться на установку SDK 35, **NDK 27.0.12077973**, CMake 3.22.1
   (при другом NDK удалите строку `ndkVersion` в `app/build.gradle.kts`)
3. Дождаться синхронизации → `Run ▶`

Без wrapper-`jar` Android Studio синхронизирует и так; из CLI:
```bash
gradle wrapper --gradle-version 8.7
./gradlew assembleDebug
```

## Управление

| Жест | Действие |
|---|---|
| 1 палец — перетаскивание | вращение |
| 2 пальца — сведение/разведение | масштаб |
| 2 пальца — перетаскивание | панорама |
| «Сетка» / «Сброс» | каркас / сброс камеры |

## Архитектура и путь данных

```
UI (Kotlin)                    C++/NDK (OpenGL ES 3.0)
────────────                   ────────────────────────
MainActivity ──жесты──────────► Camera (yaw/pitch/dist)
  │  SAF: copyTo(cache)                │
  ▼                                   ▼
NativeLib.nativeLoadModel(path) ──► native_bridge.cpp
  │                                   ├─ stl_parser (binary/ascii)
  │                                   ├─ threemf_parser (inflate+zip+xml)
  │                                   ├─ step_parser (BREP, UV-тесселяция)
  │                                   ├─ obj_parser / ply_parser
  │                                   ├─ gltf_parser (json_mini, GLB)
  │                                   └─ amf_parser (xml)
  ▼                                        ▼
GLSurfaceView.Renderer ──────► gl_renderer.cpp (VBO/EBO, 1-2 draw calls)
```

Ключевые файлы: [`inflate.cpp`](app/src/main/cpp/inflate.cpp),
[`zip_reader.cpp`](app/src/main/cpp/zip_reader.cpp),
[`step_parser.cpp`](app/src/main/cpp/step_parser.cpp),
[`gltf_parser.cpp`](app/src/main/cpp/gltf_parser.cpp),
[`gl_renderer.cpp`](app/src/main/cpp/gl_renderer.cpp),
[`native_bridge.cpp`](app/src/main/cpp/native_bridge.cpp),
[`json_mini.h`](app/src/main/cpp/json_mini.h).

## Почему это быстро

1. Парсинг целиком на C++: STL — `fread` + memcpy-скан; STEP — однопроходный скан сущностей;
   glTF — свой JSON-DOM. Без копий через JVM.
2. Рендер: интерливированный VBO (pos+normal, stride 24 Б), один EBO (треугольники + рёбра
   со смещением) → **1 draw call** (2 с каркасом).
3. Нормали/нормировка вычисляются один раз в C++.
4. `RENDERMODE_WHEN_DIRTY` — кадр только по запросу (экономия батареи).
5. Ноль зависимостей → маленький APK, быстрая сборка.

## Дорожная карта

- ✅ **Этап 1**: скелет, STL, 3MF, камера, каркас
- ✅ **Этап 2**: STEP (аналитический BREP), OBJ, PLY, glTF/GLB, AMF
- ✅ **Этап 3**: адаптивный UI, настройки (фон/свет/FOV/оси/MSAA/чувствительность),
  скриншот, двойной тап, размер модели
- 🔜 **Этап 4**: цвета/материалы (3MF basematerials, PLY/OBJ vertex color,
  glTF material index), текстуры
- 🔜 **Этап 5**: NURBS в STEP (b-spline поверхности/кривые), IGES, X3D/VRML, DXF-3D
- 🔜 **Этап 6**: mmap, дедупликация вершин, фоновая загрузка с прогрессом
- 🔜 **Этап 7**: PBR/тени, инерция и анимация камеры, измерения/сечения, экспорт OBJ/GLTF

## Предложенные (напрашивающиеся) функции

1. **Галерея файлов** (недавние модели, миниатюры через тот же рендер).
2. **Режимы отображения**: X-Ray, плоская заливка (flat), градиент по высоте/нормали.
3. **Единицы измерения** (мм/см/дюйм) и «масштаб в 1:1» для печати (окулярные размеры).
4. **Измерения**: расстояние между точками, объём, площадь поверхности.
5. **Инструменты**: сечение плоскостью, вращение вокруг произвольной оси.
6. **Плоскость основания/сетка** (ground grid) — гиро-ориентация при печати.
7. **Экспорт**: OBJ / STL / GLTF (актуально после этапа 4 с цветами).
8. **Жесты инерции** (flick = плавное вращение с затуханием).
9. **Тёмная/светлая тема UI** и выбор акцентного цвета.
10. **Мульти-селект объектов** (отдельными сетками, переключение видимости).

## Известные ограничения

- STEP: квадратичные NURBS-поверхности/кривые не поддерживаются (грани пропускаются
  со счётчиком); для дуг на боковых поверхностях используется обход "по часовой"
  параметризации — в редких случаях возможен выбор другой дуги; полюса сферы дают
  вырожденные треугольники.
- 3MF: без zip64, текстур, component-объектов.
- glTF: без внешних `.bin` файлов (только GLB/data URI), квантованные accessors и
  `indices` типа UBYTE/USHORT поддерживаются, другие типы игнорируются.
- ZIP: только STORE и DEFLATE (покрывает спецификацию 3MF).

> Примечание: красные ошибки IntelliSense про `GLES3/gl3.h`, `jni.h`, `std::sqrt`
> и т.п. — это артефакты C/C++-расширения без NDK-тулчейна. После первой
> синхронизации Gradle/CMake IntelliSense подхватит include-пути, а реальная
> сборка через clang из NDK их не имеет.

## Лицензия

Проект распространяется по лицензии [MIT](LICENSE).
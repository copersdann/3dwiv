# 3Dwiv — учебный разбор всех строк кода

Это руководство построено так, чтобы ты **читал код снизу вверх**: сначала байты и
математика, потом парсеры, потом рендер, потом JNI и UI. Каждый блок объясняет
«что происходит и почему именно так».

---

## 0. Карта чтения (порядок обучения)

1. **База C++**: [`math3d.h`](app/src/main/cpp/math3d.h) → [`camera`](app/src/main/cpp/camera.cpp) → [`mesh`](app/src/main/cpp/mesh.cpp)
2. **Рендер**: [`gl_renderer`](app/src/main/cpp/gl_renderer.cpp) + шейдеры
3. **Простые парсеры**: [`stl_parser`](app/src/main/cpp/stl_parser.cpp) → [`obj_parser`](app/src/main/cpp/obj_parser.cpp) → [`ply_parser`](app/src/main/cpp/ply_parser.cpp)
4. **Сжатие**: [`inflate`](app/src/main/cpp/inflate.cpp) → [`zip_reader`](app/src/main/cpp/zip_reader.cpp) → [`threemf_parser`](app/src/main/cpp/threemf_parser.cpp)
5. **JSON/GLTF**: [`json_mini`](app/src/main/cpp/json_mini.cpp) → [`gltf_parser`](app/src/main/cpp/gltf_parser.cpp)
6. **STEP**: [`step_parser`](app/src/main/cpp/step_parser.cpp)
7. **Мост**: [`native_bridge`](app/src/main/cpp/native_bridge.cpp)
8. **Kotlin**: [`NativeRenderer.kt`](app/src/main/java/com/threewiv/viewer/NativeRenderer.kt) → [`NativeLib.kt`](app/src/main/java/com/threewiv/viewer/NativeLib.kt) → [`MainActivity.kt`](app/src/main/java/com/threewiv/viewer/MainActivity.kt)
9. **Сборка**: `settings.gradle.kts` → `app/build.gradle.kts` → [`CMakeLists.txt`](app/src/main/cpp/CMakeLists.txt)

---

## 1. База: математика, камера, сетка

### 1.1 [`math3d.h`](app/src/main/cpp/math3d.h) — матрицы OpenGL

```cpp
inline void multiply(const float* a, const float* b, float* out)
```
- Матрицы 4×4 хранятся **по колонкам** (`out[c*4 + r]` = элемент колонки c, строки r).
  OpenGL ожидает именно такой порядок в `glUniformMatrix4fv(..., GL_FALSE, ...)`.
- `multiply` = обычное матричное произведение a·b (внимание на порядок индексов: `a[k*4+r] * b[c*4+k]`).

```cpp
inline void perspective(float fovDeg, float aspect, float zn, float zf, float* out)
```
- Классическая проекция: `f = 1/tan(fov/2)`, элемент `out[11] = -1` — это «w=-z»,
  из-за которого дальние точки получают меньший w (перспектива).
- Элементы `out[10]` и `out[14]` отображают глубину в диапазон [0;1] (NDC).

```cpp
inline void lookAt(ex,ey,ez, cx,cy,cz, ux,uy,uz, out)
```
- Строится ортонормированный базис: `f = normalize(center-eye)`,
  `s = normalize(f × up)`, `up' = s × f`.
- Первые 3 колонки = транспонированная матрица поворота, 4-я колонка = `-R·eye`
  (перенос мира в систему камеры).

### 1.2 [`camera.h`](app/src/main/cpp/camera.h) / [`camera.cpp`](app/src/main/cpp/camera.cpp) — орбитальная камера

```cpp
yaw, pitch, dist, tx/ty/tz, fovDeg
```
- Камера = точка-цель (target) + сферические координаты наблюдателя:
  `eye = target + dist*(cos(pitch)*cos(yaw), sin(pitch), cos(pitch)*sin(yaw))`.
  Это стандартная «орбита» (как колесо обзора в CAD).

```cpp
void Camera::rotate(float dxDeg, float dyDeg) { yaw -= ...; pitch += ...; clamp ±89° }
```
- Перетаскивание по X меняет `yaw`, по Y — `pitch`. Ограничение ±89° защищает от
  «опрокидывания» камеры (gimbal lock типа «встали на голову»).

```cpp
void Camera::pan(float dxPx, float dyPx, float viewH)
```
- Ключевая идея: **пиксель → мировые единицы**: `worldPerPx = 2*dist*tan(fov/2)/viewH`.
  Чем дальше камера, тем больше модель «гуляет» за пальцем (эффект перспективы).
- Базис `right = f × up`, `up' = right × f` строим из углов, чтобы двигать цель
  в плоскости экрана.

```cpp
getViewMatrix / getProjMatrix
```
- Комбинируем в рендерере: `mvp = proj * view` (умножение слева — сначала view, потом proj).

### 1.3 [`mesh.h`](app/src/main/cpp/mesh.h) / [`mesh.cpp`](app/src/main/cpp/mesh.cpp) — данные сетки

```cpp
struct MeshData { std::vector<float> positions; normals; std::vector<uint32_t> indices; }
```
- `positions` — плоский массив `x0,y0,z0, x1,y1,z1, …` (SoA vs AoS — здесь AoS по 3 float).
  Индексы — `uint32_t` (GL_UNSIGNED_INT), поддерживают >65535 вершин.
- `clear()` обязателен перед каждым парсингом, чтобы не было утечек из предыдущей модели.

```cpp
bool finalizeMesh(MeshData& m, std::string* err)
```
1. Считаем bounding box (`mnx..mxx` и т.д.).
2. `cx = (mnx+mxx)/2` — центр; `k = 2/span` — масштаб под единичный куб (полуразмер 1).
3. Применяем `p = (p - center) * k` — убираем огромные абсолютные координаты STEP,
   чтобы float-точность была максимальной.
4. **Нормали**: для каждого треугольника `n = (b-a) × (c-a)` и прибавляем к трём
   вершинам. Сумма необработанных (не нормированных) cross-произведений даёт
   «площадевзвешенное» сглаживание: большие грани влияют сильнее.
5. Нормируем каждую нормаль; нулевые (вырожденные треугольники) получают `(0,1,0)`.

> Почему нормали считаем сами? STL даёт нормали граней (их можно проигнорировать —
  они часто мусорные), а для плавного освещения нужны **вершинные** нормали.
  Расчёт один раз в C++, GPU получает готовые данные.

---

## 2. Рендер: [`gl_renderer.cpp`](app/src/main/cpp/gl_renderer.cpp)

### 2.1 Шейдеры (внутри `init()`)

```glsl
#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 uMvp;
uniform mat4 uView;
out vec3 vNormal;
```
- `layout(location=N)` — привязка к атрибутам **в шейдере**, чтобы `glVertexAttribPointer`
  с тем же location не зависел от имён.
- `vNormal = mat3(uView) * aNormal` — переводим нормаль в пространство камеры,
  где свет задан фиксированно (`vec3(0.35, 0.5, 0.8)` — направление света).
  `mat3` отбрасывает перенос — нормали не должны сдвигаться.

Фрагментный шейдер:
```glsl
float d = max(dot(n, lightDir), 0.0);
float light = uAmbient + (1.0 - uAmbient) * d;
```
- Простейшая модель Ламберта: диффузная + ambient. `uAmbient=0.28` даёт освещение
  «с подсветкой теней», чтобы модель не была чёрной со стороны от света.

### 2.2 [`compileShader` / `linkProgram`](app/src/main/cpp/gl_renderer.cpp)

- Проверка `GL_COMPILE_STATUS` / `GL_LINK_STATUS` + вывод лога в Logcat — обязательная
  дисциплина: без неё ошибка шейдера = молчаливый чёрный экран.

### 2.3 [`setMesh`](app/src/main/cpp/gl_renderer.cpp) — загрузка в GPU

```cpp
std::vector<float> inter; inter.resize(vc * 6);
// inter[i*6+0..2] = позиция, inter[i*6+3..5] = нормаль
```
- **Интерливинг**: чередуем атрибуты в одном VBO (позиция 12 байт + нормаль 12 байт
  = шаг 24 байта). Так GPU читает оба атрибута одним кэш-лайн-запросом вместо двух.

```cpp
// idx: сначала треугольники (tc*3), затем рёбра для каркаса (tc*6)
idx[o+0]=a; idx[o+1]=b; idx[o+2]=b; idx[o+3]=c; idx[o+4]=c; idx[o+5]=a;
```
- **Один EBO на всё**: в него кладём и индексы треугольников, и рёбра. Каркас
  рисуется `glDrawElements(GL_LINES, ..., (void*)(tc*3*4))` — просто со смещением!
  Плюс ноль дополнительных вызовов и буферов.

```cpp
glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
```
- `stride = 24` (байт), `offset = 0/12` — разбор интерливинга. `GL_STATIC_DRAW`
  говорит драйверу: данные не меняются — можно держать в VRAM.

### 2.4 [`render()`](app/src/main/cpp/gl_renderer.cpp)

```cpp
glClearColor(...); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
```
- Depth test — чтобы дальние треугольники не рисовались поверх ближних.
- Cull back — отбрасываем задние грани: ~×2 быстрее растеризации. `GL_LEQUAL`
  уменьшает артефакты z-боя на совпадающих гранях.

Два draw call'а подряд:
```cpp
glDrawElements(GL_TRIANGLES, tc*3, GL_UNSIGNED_INT, nullptr);
if (wireframe) glDrawElements(GL_LINES, tc*6, GL_UNSIGNED_INT, offset);
```
- Заливка + каркас. Slivers между рёбрами нет, потому что рёбра рисуются
  поверх граней с тем же depth-тестом (ближе — видно).

---

## 3. JNI-мост: [`native_bridge.cpp`](app/src/main/cpp/native_bridge.cpp)

### 3.1 Имена функций

```cpp
extern "C" JNIEXPORT jboolean JNICALL
Java_com_threewiv_viewer_NativeLib_nativeInit(JNIEnv*, jclass)
```
- Правило манглинга JNI: `Java_<пакет с _ вместо .>_<Класс>_<метод>`.
- `jclass` — потому что Kotlin `object NativeLib` с `@JvmStatic` делает методы
  **статическими** (см. [`NativeLib.kt`](app/src/main/java/com/threewiv/viewer/NativeLib.kt)).
- Первый параметр всегда `JNIEnv*` (указатель на таблицу функций JNI), второй —
  `jclass` (статический) или `jobject` (экземплярный).

### 3.2 Потоковая модель

```cpp
std::mutex g_lock; GlRenderer* g_gl; MeshData g_current; std::atomic<bool> g_dirty;
```
- **GL-поток** (GLSurfaceView) строит ресурсы и рисует; **UI-поток** крутит камеру
  и грузит файлы. Мьютекс синхронизирует, а `g_dirty` — флаг «есть новая сетка».
- В `nativeRender`: если `g_dirty` → `g_gl->setMesh(g_current)`. Так парсинг (тяжёлый)
  не блокирует GL, а аплоад в GPU идёт ровно один раз на GL-потоке.

```cpp
if (g_gl) { g_gl->destroy(); delete g_gl; ... }  // в nativeInit
```
- При пересоздании поверхности (например, после паузы) GL-контекст новый — старые
  VBO недействительны. Поэтому уничтожаем и **перезаливаем `g_current`** (CPU-копия
  хранится именно ради этого).

### 3.3 Строки и ошибки

```cpp
const char* cpath = env->GetStringUTFChars(jpath, nullptr);
...
env->ReleaseStringUTFChars(jpath, cpath);
env->NewStringUTF(res.c_str());
```
- `GetStringUTFChars` — красиво для чтения, но **нужен Release** (иначе утечка
  локального буфера). `NewStringUTF` создаёт java-строку (UTF-8 → modified UTF-8).

### 3.4 Детект формата

```cpp
glbMagic = bytes[0]=='g' && bytes[1]=='l' && bytes[2]=='T' && bytes[3]=='F';
stepMagic = startsWith("ISO-10303-21");
amfMarker = ищем "<amf" в первых 8 КБ;
```
- Сигнатуры сильнее расширения (пользователь мог переименовать файл). ZIP `PK…`,
  `ply`, `solid`, `glTF` — стандартные magic-байты форматов.

---

## 4. Парсеры. Общие приёмы (важно для обучения)

1. **Никаких регулярных выражений и библиотек** — байтовый скан по указателям.
2. **Числа через `strtof`/`strtoul`** с копированием в локальный буфер
   (функции требуют `\0`-терминатор, а мы парсим кусок файла).
3. **LE-чтение вручную**: `b[0] | b[1]<<8` — не зависим от порядка байт CPU
   (Android — little-endian, но привычка правильная).
4. **Безопасность**: каждая проверка `n < count`, `offset + size <= буфер`,
   капы (`kMaxOut`, `kVertLimit`) — защита от «zip-бомб» и битых файлов.
5. Итоговый результат всегда прогоняется через `finalizeMesh`.

### 4.1 [`stl_parser.cpp`](app/src/main/cpp/stl_parser.cpp)

**Binary** (`parseBinary`):
```cpp
uint32_t n = readU32(d + 80);                      // кол-во треугольников после 80 байт
size_t expect = 84ull + (size_t)n * 50ull;          // размер, если файл валиден
```
- Каждый треугольник = нормаль (12 Б) + 3 вершины (36 Б) + attribute (2 Б) = 50 Б.
- Проверка `expect <= size` (±2 КБ хвоста) — отличает binary от ASCII, у которого
  такой размер никогда не совпадёт.
- Вершины читаем **прямым `memcpy` в float** — без распаковки байт вручную
  (просто и очень быстро).
- Нормали из файла пропускаем (`p += 12`) — пересчитаем сами.

**ASCII** (`parseAscii`):
- Токены `solid/facet/outer/loop/vertex/endloop/endfacet` и числа.
- Наткнулись на `vertex` — читаем 3 числа; на `endfacet` (или `endsolid`) —
  если накоплено 3 вершины, эмитим треугольник.
- Игнорируем всё остальное — формат прощает лишние строки.

### 4.2 [`obj_parser.cpp`](app/src/main/cpp/obj_parser.cpp)

```cpp
if (c0 == 'v' && isWs(p[1])) { ... }   // «v » — это позиция, а «vt»/«vn» — нет
```
- Различаем `v`/`vt`/`vn` по следующему символу — необычный, но важный трюк.
- `readFaceIndex`: парсим `12`, `12/1`, `12/1/2`, `12//2`, отрицательный индекс
  `-3` = «с конца»: `vi = total + v` (v уже отрицательный).
- n-угольник → веер `(0, i, i+1)` — простая триангуляция выпуклых многоугольников.

### 4.3 [`ply_parser.cpp`](app/src/main/cpp/ply_parser.cpp)

- Заголовок: `element vertex N`, `property <type> <name>`, `property list <ct> <vt> <name>`.
- `typeCode` отображает имена типов на коды 1..8; `typeSize` — размер в байтах.
- `Reader::val(type, val)` — единая точка чтения: работает и с little, и с big
  endian (`rd16le/rd16be/rd32le/rd32be`); float/double читаются через `memcpy` из
  uint32/uint64 (никаких UB при нарушении выравнивания).
- ASCII-ветка читает по токенам, binary-ветка — строго по `props` в порядке
  объявления (нельзя переставлять!).

### 4.4 [`inflate.cpp`](app/src/main/cpp/inflate.cpp) — DEFLATE (самый интересный)

**BitReader**:
```cpp
uint32_t peek(int n) { while (cnt<n) buf |= data[pos++] << cnt; }
void drop(int n) { buf >>= n; cnt -= n; }
```
- DEFLATE читается **LSB-первым** внутри байта. `buf` — аккумулятор, `cnt` — сколько
  бит накоплено. Заливаем по байту, отдаём младшие биты.

**Канонический код Хаффмана** (`Huff::build`):
```cpp
int code = 0;
for (len: 1..15) { for (sym с длиной len) { код[sym]=code; code++; } code <<= 1; }
```
- Правило: короткие коды имеют меньшие значения; код длины L начинается с
  `(последний код длины L-1 + 1) << 1`. Так коды однозначны без дерева.
- **Реверс бит**: поток передаёт код MSB-первым, а мы читаем LSB-первым, поэтому
  `table[reverseBits(code,len) | (k<<len)] = (len<<16)|sym` — заполняем все
  «продолжения» кода. Декодер: `peek(15)` → `table[v&0x7FFF]` → `drop(len)`.

**Три типа блоков**:
- `00` stored: выравнивание на байт = `buf=0;cnt=0`, затем `LEN/NLEN` и копия.
- `01` fixed: длины по RFC (0–143:8, 144–255:9, 256–279:7, 280–287:8).
- `10` dynamic: читаем длины кодов для кодов длин (18 символов, табл. `kClOrder`),
  итеративно распаковываем 16/17/18 (повторы).

**LZ77** (`decodeBlock`):
```cpp
int len = kLenBase[li] + br.read(kLenExtra[li]);
int dist = kDistBase[dsym] + br.read(kDistExtra[dsym]);
for (i<len) out[here+i] = out[here-dist+i];
```
- Таблицы `kLenBase/kLenExtra/kDistBase/kDistExtra` — прямо из RFC 1951 (таблицы 3,4):
  kod+доп.бит → длина/дистанция. Копия побайтово (перекрытие возможно — dist < len).

**Защита**: `kMaxOut = 1GiB` — прерываем распаковку, если выход разросся (zip-бомба).

### 4.5 [`zip_reader.cpp`](app/src/main/cpp/zip_reader.cpp)

- **EOCD** ищем с конца (последние 64 КБ разрешён комментарий): сигнатура
  `0x06054b50`, оттуда: число записей, размер и офсет центрального каталога.
- **Central directory**: каждая запись сигнатура `0x02014b50`; по имени (без учёта
  регистра) ищем нужный файл; берём `method`, `compSize`, `localOff`.
- **Local header**: `0x04034b50`, пропускаем имена/extra, отдаём сжатые байты в
  `inflate::raw` (метод 8) или копируем (метод 0).

### 4.6 [`threemf_parser.cpp`](app/src/main/cpp/threemf_parser.cpp)

- `getAttr`: ищем `имя=` **с границей** (перед именем пробел или начало тега) —
  иначе `"id"` совпадёт внутри `"objectid"`.
- `tagBase`: отрезаем префикс `m:vertex` → `vertex`.
- Собираем `objects` (id, вершины, индексы) и `items` (objectid + transform).
- `appendObject` применяет матрицу 3×4: `p' = M·(p,1)`; индексы сдвигаются на base.
- Если `<build>` пуст — берём все объекты.

### 4.7 [`json_mini.cpp`](app/src/main/cpp/json_mini.cpp)

- Рекурсивный спуск: `{` → `parseObj`, `[` → `parseArr`, строка/число/true/false/null.
- Escape-последовательности включая `\uXXXX` (ручная `appendUtf8`).
- `Node` — простой DOM: упорядоченные пары для объектов, массив — для массивов.

### 4.8 [`gltf_parser.cpp`](app/src/main/cpp/gltf_parser.cpp)

- **GLB**: заголовок `glTF` + версия 2 + чанки `JSON`/`BIN\0`. Чанк JSON парсим,
  BIN — байты буфера.
- **accessor**: `bufferView→buffer`, `byteOffset`, `componentType` (5126=float),
  `count`, `type=VC3`, `byteStride`. Чтение со stride — так поддерживаются упакованные
  данные.
- **node-иерархия**: DFS со стеком; мировая матрица = родитель × локальная.
  Локальная: `matrix[16]` (column-major по спецификации glTF) или TRS;
  кватернион→матрица по стандартной формуле.
- Примитивы `mode` 4 (triangles), 5 (strip), 6 (fan) — и без индексов.

### 4.9 [`step_parser.cpp`](app/src/main/cpp/step_parser.cpp) — самый сложный

**Сущности**:
```cpp
// исходный файл: #5=ADVANCED_FACE('',( #1,... ),#7,.T.);
// скан: '#' → id, '=' → CLASS, '(' → баланс до ')', ';'
```
- Учитываем строки `'...'` (внутри может быть `;`, `(`, `)`), поэтому скан
  сбалансирован с пропуском строк — это типичная проблема при парсинге STEP.

**Система типов**:
- `Placement` (o, x, y, z) — из `AXIS2_PLACEMENT_3D`; z=ось, x=ref_direction
  с удалением z-компоненты, y = z×x.
- `Surf` + `evalSurf(u,v)` для 5 поверхностей (см. формулы ниже) и `uvinv(xyz)`
  — обратные отображения:
  - plane: `u=dx, v=dy`;
  - cylinder: `u=atan2(dy,dx)`, `v=dz`;
  - cone: `r=v·tan(angle)`, `u=atan2`, `v=dz`;
  - sphere: `v=asin(dz/R)`, `u=atan2(dy,dx)`;
  - torus: `v=asin(dz/r)`, `u=atan2(dy,dx)`.

**Грань** (`ADVANCED_FACE`):
1. loops → каждая грань = набор `FACE_BOUND`/`FACE_OUTER_BOUND` → `EDGE_LOOP`.
2. `ORIENTED_EDGE` → `EDGE_CURVE` → кривая + вершины.
3. Для circle: `delta = fmod(a1-a0+2π, 2π)` (обход по часовой параметризации),
   количество сегментов = `delta / (2π/48)` (48 сегментов на оборот).
4. Проецируем точки кривой в UV поверхности (`uvinv`), `unwrap` разрывы шва
   (скачки > π), `collapseDups` убирает дубли.
5. Внешняя петля = max |площадь|, отверстия — противоположная намотка.

**Триангуляция с отверстиями** (`bridgeAndEarClip`):
- Для каждого отверстия: верхняя точка → ищем видимую вершину внешней цепочки
  (середина отрезка внутри полигона, без пересечений с рёбрами) → «мост».
- Сшиваем (bridge + возврат через все вершины отверстия) → одна петля.
- **Ear clipping**: пока >3 вершин, ищем «ухо» (выпуклую вершину, внутри которой
  нет других точек), вырезаем треугольник. Форс-клип при нуле найденных ушей.

**Выравнивание нормалей**:
- Для треугольника сравниваем `cross(p1-p0, p2-p0)` с аналитической нормалью
  поверхности в центре UV; при расхождении разворачиваем — так модель получит
  наружу направленные нормали даже если намотка loop'а нестандартная.

---

## 5. Kotlin-часть

### 5.1 [`NativeLib.kt`](app/src/main/java/com/threewiv/viewer/NativeLib.kt)

```kotlin
object NativeLib {
    init { System.loadLibrary("threewiv") }
    @JvmStatic external fun nativeRotate(dx: Float, dy: Float)
}
```
- `external fun` = объявление без тела: вызов уйдёт в JNI.
- `@JvmStatic` обязателен: без него методы `object` — экземплярные, и JNI-сигнатура
  была бы `jobject`, а наша C-функция ждёт `jclass`.
- `System.loadLibrary("threewiv")` — загрузка `libthreewiv.so` из APK.

### 5.2 [`NativeRenderer.kt`](app/src/main/java/com/threewiv/viewer/NativeRenderer.kt)

- Класс-`GLSurfaceView.Renderer`, методы которого вызываются на **специальном
  GL-потоке** (не UI!). Именно поэтому там безопасно создавать/удалять GL-ресурсы.
- `onSurfaceCreated` → init; `onSurfaceChanged` → resize; `onDrawFrame` → render.

### 5.3 [`MainActivity.kt`](app/src/main/java/com/threewiv/viewer/MainActivity.kt)

```kotlin
glView.setEGLContextClientVersion(3)          // OpenGL ES 3.0
glView.renderMode = GLSurfaceView.RENDERMODE_WHEN_DIRTY  // кадр только по запросу
```
- `WHEN_DIRTY` + ручные `requestRender()` после жеста/загрузки = экономия батареи
  (нет постоянной 60 FPS вхолостую).

**Жесты**:
- `ACTION_DOWN` → режим «вращение», `ACTION_POINTER_DOWN` → «пинч» (2 пальца).
- В `ACTION_MOVE`: вращение = `nativeRotate(dx,dy)`; пинч = отношение расстояний
  `prevPinch / d` (`nativeZoom`), панорама = смещение середины (`nativePan`).
- `ev.actionMasked` — правильный способ ловить мультитач.

**Файлы (SAF)**:
```kotlin
Intent(Intent.ACTION_OPEN_DOCUMENT).apply { addCategory(CATEGORY_OPENABLE); type = "*/*" }
```
- Storage Access Framework даёт `content://` URI **без разрешений на хранилище**.
- Копируем в `cacheDir` (чтобы отдать C++ путь), парсим в фоновом `Thread`,
  результат — в `runOnUiThread`.

---

## 6. Сборка и CMake

- `settings.gradle.kts` — репозитории + список модулей (`:app`).
- `app/build.gradle.kts`:
  - `ndkVersion` / `abiFilters ("arm64-v8a","x86_64")` — 64-битные сборки;
  - `externalNativeBuild.cmake` — путь к CMakeLists и версия;
  - `cppFlags += ["-O2"]` — оптимизация; `ANDROID_ARM_NEON=ON` — NEON-инструкции.
- [`CMakeLists.txt`](app/src/main/cpp/CMakeLists.txt):
  - `add_library(threewiv SHARED ...список .cpp...)` — создаёт `libthreewiv.so`;
  - `target_link_libraries(threewiv android log GLESv3)` —
    `log` для `__android_log_print`, `GLESv3` для OpenGL ES 3.0.

---

## 7. Глоссарий

| Термин | Значение |
|---|---|
| VBO | Vertex Buffer Object — массив вершин на GPU |
| VAO | Vertex Array Object — «рецепт» разбора атрибутов (location+stride+offset) |
| EBO | Element Buffer Object — индексы треугольников |
| UNSIGNED_INT | 4-байтный индекс: >65535 вершин |
| draw call | вызов `glDrawElements` (один треугольник — одна команда) |
| NDC | Normalized Device Coordinates, [-1..1]³ |
| UV | параметры поверхности (углы/длины, а не координаты) |
| BREP | Boundary Representation: тело задано гранями/кромками/вершинами |
| DEFLATE | алгоритм сжатия: Хаффман + LZ77 (RFC 1951) |
| magic | первые байты файла, по которым узнают формат |
| JNI | Java Native Interface — вызов C++ из Kotlin/Java |
| interleave | чередование атрибутов в одном буфере |
| ear clipping | триангуляция многоугольника «отрезаем уши» |

## 8. Как отлаживать

```bash
adb logcat -s 3dwiv:*            # тэги из native_bridge/gl_renderer
adb logcat -s AndroidRuntime:E   # crash/UnsatisfiedLinkError из JNI
adb shell run-as com.threewiv.viewer ls files/   # кэш (для отладки путей)
```
- `UnsatisfiedLinkError` → не совпало имя JNI или забыли `@JvmStatic`.
- Чёрный экран после загрузки → смотри `GL_COMPILE_STATUS` лог в Logcat.
- Падение при парсинге → включи `-fsanitize=address` в `cppFlags` временно.

## 9. Упражнения (нарастающая сложность)

1. Добавь кнопку «Сброс» с удержанием, чтобы камера анимировалась (lerp) до начальной.
2. В `gl_renderer.cpp` добавь второй источник света (uniform `uLight2`).
3. В STL: дедупликация вершин по точному равенству (unordered_map ключ = 3 float).
4. Добавь «плоские» (flat) нормали в шейдере: `flat out vec3 vNormal;`.
5. В PLY: поддержка `element edge` (рисовать линиями).
6. В OBJ: парсинг `vn` и использование их вместо пересчёта.
7. В GLTF: поддержка vertex colors (атрибут `COLOR_0` → uniform на сторону).
8. В STEP: добавь `B_SPLINE_CURVE_WITH_KNOTS` с де Boор-алгоритмом (см. теорию NURBS).

## 10. Что дальше (архитектурные уроки)

- **Парсинг → нормализация → GPU** — разделяй, чтобы можно было грузить в фоне.
- **Один буфер на всё** (треугольники+рёбра) — пример «окупаемой» оптимизации.
- **Магические числа держи константами** (`kSeg=48`, `kMaxOut`, `kVertLimit`).
- **Тест на мусоре**: переименованные/обрезанные файлы — парсер должен вернуть
  ошибку, а не упасть. Каждая выборка из массива — с проверкой границ.
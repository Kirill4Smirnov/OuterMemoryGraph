# Полное руководство по проекту Outer Memory Graph

Этот документ описывает проект целиком: исходную задачу, принятые инженерные
решения, устройство кода, алгоритм предварительной обработки и PageRank,
форматы данных, команды запуска, способы проверки результатов, измерение памяти,
известные ограничения и направления дальнейшего развития.

Если нужно только быстро собрать и запустить программу, начните с раздела
[«Быстрый старт»](#13-быстрый-старт).

## Содержание

- [1. Что было сделано](#1-что-было-сделано)
- [2. Какую задачу решает программа](#2-какую-задачу-решает-программа)
- [3. Почему выбран PageRank](#3-почему-выбран-pagerank)
- [4. Почему выбран Destination-sharded pull](#4-почему-выбран-destination-sharded-pull)
- [5. Общая архитектура](#5-общая-архитектура)
- [6. Формула и семантика PageRank](#6-формула-и-семантика-pagerank)
- [7. Предварительная обработка по шагам](#7-предварительная-обработка-по-шагам)
- [8. Внутренний формат графа](#8-внутренний-формат-графа)
- [9. Одна итерация PageRank](#9-одна-итерация-pagerank)
- [10. Многопоточность и детерминизм](#10-многопоточность-и-детерминизм)
- [11. Управление памятью](#11-управление-памятью)
- [12. Гипер-узлы](#12-гипер-узлы)
- [13. Быстрый старт](#13-быстрый-старт)
- [14. Полный справочник CLI](#14-полный-справочник-cli)
- [15. Проверка результатов](#15-проверка-результатов)
- [16. Тесты и quality gate](#16-тесты-и-quality-gate)
- [17. Бенчмарки и фактические результаты](#17-бенчмарки-и-фактические-результаты)
- [18. Структура исходного кода](#18-структура-исходного-кода)
- [19. Ошибки и диагностика](#19-ошибки-и-диагностика)
- [20. Ограничения](#20-ограничения)
- [21. Что можно улучшить](#21-что-можно-улучшить)

## 1. Что было сделано

Работа над заданием включала не только реализацию формулы PageRank, но и полный
контур обработки графа, который больше доступной оперативной памяти.

В проекте выполнены следующие части:

1. Исследованы PageRank, LeaderRank, Katz, HITS, Jaccard, Dice и несколько
   архитектур внешнепамятной обработки.
2. В качестве метрики выбран PageRank, а в качестве основной архитектуры —
   Destination-sharded semi-external pull.
3. Реализован потоковый парсер CSV и SNAP edge-list.
4. Реализована внешняя сортировка с ограниченным объёмом памяти:
   chunk-based run generation, локальная дедупликация и bounded-fan-in merge.
5. Исходные signed `int32` ID перенумеровываются в плотный диапазон `uint32`.
6. На SSD строятся шарды, отсортированные по `(destination, source)`.
7. Дубликаты направленных рёбер удаляются, self-loop сохраняются.
8. Реализован PageRank с корректной обработкой dangling vertices.
9. Итерационная фаза распараллелена по независимым destination-шардам.
10. Исключены floating-point atomics и недетерминированные конфликтующие записи.
11. Добавлены фиксированный и convergence-режимы остановки.
12. Добавлены memory preflight, проверки внутреннего формата и атомарная
    публикация графа и выходного CSV.
13. Подготовлены `shell.nix`, CMake-проект и Ubuntu 22.04 Docker-образ.
14. Добавлены unit/integration tests, официальный LDBC Graphalytics fixture,
    тест гипер-узла и проверка детерминизма.
15. Проведены эксперименты на `cit-Patents` и `soc-LiveJournal1`, включая
    thread scaling и запуск в cgroup с лимитом 128 MiB без swap.
16. Подготовлены research-документ, технический отчёт, спецификация формата и
    скрипты валидации и измерений.

## 2. Какую задачу решает программа

Вход программы — невзвешенный ориентированный граф, заданный списком рёбер:

```text
source -> destination
```

Граф может занимать сотни мегабайт или гигабайты, тогда как процессу доступно,
например, 128 MiB RAM. Поэтому обычный подход вида
`vector<vector<Vertex>> adjacency` использовать нельзя.

Программа разделена на две самостоятельные стадии:

1. `preprocess` преобразует текстовый edge-list во внутренний бинарный формат.
2. `pagerank` многократно читает полученные шарды и считает ранги.

Это разделение осознанное. CSV удобен как формат обмена, но плохо подходит для
десятков последовательных итераций:

- числа приходится каждый раз заново разбирать из текста;
- размер CSV существенно больше бинарного представления;
- строковая обработка расходует CPU;
- исходный порядок рёбер обычно не соответствует нужному pull-обходу.

После preprocessing каждое ребро занимает ровно восемь байт и читается в
порядке, удобном для PageRank.

## 3. Почему выбран PageRank

PageRank оказался наиболее подходящей метрикой по совокупности причин:

- это глобальная метрика для всего ориентированного графа;
- одна итерация имеет линейную сложность `O(V + E)`;
- результат имеет размер `O(V)`, а не `O(V²)`;
- рёбра можно обрабатывать последовательным потоком;
- метрика имеет понятную практическую интерпретацию;
- существуют независимые эталонные реализации и Graphalytics reference output;
- damping factor делает оператор сжимающим и обеспечивает единственное
  стационарное распределение.

Другие рассмотренные варианты:

| Метрика | Почему не выбрана |
|---|---|
| LeaderRank | Добавляет ground node, связанный со всеми вершинами, то есть искусственный гипер-узел степени `V`; меньше стандартных эталонов. |
| Katz | Требует выбирать `alpha < 1 / rho(A)`, а значит решать дополнительную спектральную задачу. |
| Jaccard/Dice | Это метрики пар вершин; расчёт для всех пар может породить `O(V²)` результатов. |
| HITS | Нужны два связанных вектора hub/authority; PageRank проще валидировать и объяснять. |
| Betweenness | Точный расчёт слишком дорог для выбранного масштаба. |

Подробное сравнение находится в [`RESEARCH.md`](../RESEARCH.md).

## 4. Почему выбран Destination-sharded pull

В pull-модели каждая destination-вершина собирает вклад своих входящих соседей:

```text
next_rank[destination] += rank[source] / outdegree[source]
```

Рёбра на диске сортируются сначала по destination, затем по source. После этого
непрерывные диапазоны destination-вершин распределяются по файлам-шардам.

Главное свойство разбиения:

> Каждый dense destination ID принадлежит ровно одному шарду.

Из этого следуют основные преимущества:

- поток читает файл почти полностью последовательно;
- только один worker пишет конкретный диапазон `next_rank`;
- в горячем цикле не нужны mutex и floating-point atomics;
- не нужен приватный массив рангов размера `V` на каждый поток;
- после итерации не требуется сливать конфликтующие обновления;
- порядок сложения вкладов одной вершины фиксирован;
- adjacency list гипер-узла не загружается целиком.

Рассматривались также рассылка обновлений с последующим сбором и двумерное
разбиение матрицы. Оба способа позволяют вынести на диск состояние вершин, но
требуют больше временной записи или повторного чтения. Для ограничения 128 МиБ
выбран сбор входящих вкладов: он не записывает промежуточное значение для
каждого ребра.

## 5. Общая архитектура

Полный поток данных выглядит так:

```text
                         первый проход
edges.csv / SNAP  ------------------------------+
       |                                         |
       |                         endpoint ID chunks
       |                                         v
       |                                  vertex sorted runs
       |                                         |
       |                                  bounded k-way merge
       |                                         v
       |                                    vertices.bin
       |                                         |
       |                                  original -> dense map
       |                                         |
       +--------------- второй проход -----------+
                                                 |
                                      (destination, source) chunks
                                                 v
                                          edge sorted runs
                                                 |
                                          bounded k-way merge
                                                 v
                           +---------------- destination shards
                           |                     |
                           |                     +-- outdegrees.bin
                           |                     +-- manifest.json
                           v
                parallel pull PageRank
                           |
                           v
                     pagerank.csv
```

В RAM остаётся состояние размера `O(V)`. Структура рёбер размера `O(E)` после
preprocessing находится на SSD.

## 6. Формула и семантика PageRank

Для вершины `v` используется формула:

$$
r_{t+1}(v) =
\frac{1-d}{N}
+ d \sum_{u \to v}\frac{r_t(u)}{\deg^+(u)}
+ \frac{d}{N}\sum_{w:\deg^+(w)=0}r_t(w),
$$

где:

- `N = |V|` — число вершин;
- `d` — damping factor, по умолчанию `0.85`;
- `deg⁺(u)` — исходящая степень source-вершины;
- последнее слагаемое равномерно возвращает массу dangling vertices.

Начальное распределение равномерное:

$$
r_0(v)=\frac{1}{N}.
$$

### Зачем обрабатывать dangling vertices

Dangling vertex — вершина без исходящих рёбер. Если её ранг не
перераспределять, масса будет исчезать, сумма PageRank перестанет равняться
единице, а результат не будет стационарным распределением.

На каждой итерации программа последовательно вычисляет:

```text
dangling_mass = sum(rank[v] for outdegree[v] == 0)
base = (1 - damping) / N + damping * dangling_mass / N
```

Каждая вершина сначала получает `base`, затем к ней добавляются вклады входящих
рёбер.

### Режимы остановки

Поддерживаются два режима:

1. Фиксированное число итераций `--iterations N`.
2. Остановка, когда
   `sum(abs(next_rank[v] - rank[v])) <= tolerance`.

Фиксированный режим нужен для воспроизведения Graphalytics. Convergence-режим
удобнее для получения практически сошедшегося результата.

Residual между соседними итерациями не является прямой ошибкой относительно
неизвестного fixed point. Для оператора со степенью сжатия `d` можно использовать
апостериорную оценку порядка:

$$
\lVert r_t-r^*\rVert_1
\leq
\frac{\lVert r_t-r_{t-1}\rVert_1}{1-d}.
$$

## 7. Предварительная обработка по шагам

Команда `preprocess` читает исходный файл два раза. Исходный файл должен
оставаться неизменным на протяжении обоих проходов.

### 7.1. Валидация параметров

До создания результата проверяется следующее:

- все обязательные пути заданы;
- input существует и является обычным файлом;
- memory budget не меньше 32 MiB;
- число потоков и шардов положительно;
- целевой `graph-dir` ещё не существует;
- `work-dir` не совпадает с `graph-dir` и не находится внутри него.

Последнее условие важно: временный каталог внутри публикуемого графа мог бы быть
случайно перемещён или удалён вместе с ним.

### 7.2. Первый проход: vertex runs

Из каждого ребра извлекаются два исходных ID. В памяти накапливается ограниченный
массив идентификаторов. Когда массив заполнен:

1. ID сортируются.
2. Дубликаты внутри chunk удаляются.
3. Chunk записывается как binary sorted run.
4. Вектор освобождается для следующей порции.

В память не загружается весь input и не строится hash set размера `V`.

### 7.3. Bounded-fan-in merge вершин

Sorted runs сливаются через priority queue. Одновременно открывается не более 64
входных runs. Один reader использует буфер 64 KiB, поэтому входные merge-буферы
занимают около 4 MiB независимо от полного числа runs.

Если файлов больше 64, выполняются промежуточные merge-проходы. После успешного
создания объединённого файла исходные временные runs этой группы удаляются.

Финальный результат — `vertices.bin`, содержащий уникальные исходные IDs в
возрастающем порядке.

### 7.4. Dense ID mapping

Позиция ID в `vertices.bin` становится его dense ID:

```text
vertices.bin[dense_id] = original_id
```

Это позволяет хранить source и destination в `uint32`, а массивы рангов и
степеней индексировать напрямую.

Используются две стратегии original-to-dense mapping:

1. Direct mapping, если диапазон исходных ID достаточно компактный и помещается
   в budget.
2. Бинарный поиск по загруженному отсортированному `vertices.bin`, если ID
   разреженные.

Direct mapping работает как:

```text
dense = map[original - minimum_original_id]
```

Hash table намеренно не используется: её overhead сложнее точно ограничить, а
потребление памяти зависит от load factor и реализации стандартной библиотеки.

### 7.5. Второй проход: edge runs

Исходный edge-list читается повторно. Для каждого ребра:

1. `source` и `destination` переводятся в dense IDs.
2. На диске ребро представляется как `(destination, source)`.
3. Ограниченный chunk рёбер сортируется лексикографически.
4. Дубликаты внутри chunk удаляются.
5. Chunk записывается как edge run.

Порядок `(destination, source)`, а не `(source, destination)`, выбран специально
для будущего pull-обхода.

### 7.6. Merge edge runs и построение шардов

Число входных edge runs также сначала сводится к максимуму 64 bounded merge.
Финальный merge одновременно выполняет несколько задач:

- удаляет дубликаты между разными runs;
- считает исходящую степень каждого source;
- считает self-loop;
- формирует примерно сбалансированные по числу рёбер шарды;
- записывает данные manifest.

Граница шарда переносится только между разными destination IDs. Входящие рёбра
одной вершины никогда не разделяются между двумя worker-owned диапазонами.

### 7.7. Атомарная публикация

Граф сначала строится в staging directory рядом с будущим `graph-dir`:

```text
graph.omg.building-PID-TIMESTAMP
```

После записи программа проверяет manifest, размеры файлов, диапазоны шардов и
суммарное число рёбер. Только после этого staging directory атомарно
переименовывается в requested `graph-dir`.

При исключении временный session directory и staging directory удаляются RAII
guard-объектами. Поэтому незавершённый граф не выглядит готовым.

## 8. Внутренний формат графа

Каталог готового графа содержит:

```text
graph.omg/
    manifest.json
    vertices.bin
    outdegrees.bin
    shard-00000.bin
    shard-00001.bin
    ...
```

### `vertices.bin`

Raw little-endian массив `int32`:

```text
original_id[dense_id]
```

Размер файла должен быть ровно `4 * V` байт.

### `outdegrees.bin`

Raw little-endian массив `uint32`:

```text
outdegree[dense_id]
```

Размер также равен `4 * V` байт.

### `shard-NNNNN.bin`

Последовательность восьмибайтовых записей:

```text
uint32 destination
uint32 source
```

Рёбра строго отсортированы по `(destination, source)`. Дубликатов нет.

### `manifest.json`

Manifest содержит:

- версию и имя формата;
- byte order;
- абсолютный путь исходного файла;
- число вершин;
- число исходных и уникальных рёбер;
- число удалённых дубликатов;
- число self-loop;
- имена, диапазоны destination и число рёбер каждого шарда.

Пример сокращённого manifest:

```json
{
  "version": 1,
  "format": "omg-destination-sharded",
  "byte_order": "little",
  "vertex_count": 3774768,
  "input_edge_count": 16518948,
  "edge_count": 16518948,
  "duplicate_edge_count": 0,
  "self_loop_count": 1,
  "shards": [
    {
      "file": "shard-00000.bin",
      "vertex_begin": 0,
      "vertex_end": 156344,
      "edge_count": 258109
    }
  ]
}
```

Version 1 предназначена для little-endian Linux x86-64. Размер `DiskEdge`
зафиксирован compile-time `static_assert` и равен восьми байтам.

Подробная спецификация находится в [`file-format.md`](file-format.md).

## 9. Одна итерация PageRank

Перед первой итерацией программа:

1. Загружает и валидирует manifest.
2. Проверяет размеры всех бинарных файлов.
3. Вычисляет ожидаемый объём памяти.
4. Загружает `outdegrees.bin`.
5. Создаёт два массива `double`: `rank` и `next_rank`.
6. Заполняет `rank` значением `1 / V`.

Далее каждая итерация выполняет:

1. Последовательное вычисление dangling mass.
2. Расчёт общей базовой части `base`.
3. Раздачу destination-шардов worker-потокам.
4. Заполнение принадлежащего шарду диапазона `next_rank` значением `base`.
5. Последовательное чтение рёбер шарда блоками по 128 KiB.
6. Добавление вклада source к destination.
7. Вычисление локальных `L1 residual` и суммы рангов.
8. Детерминированное сведение локальных результатов главным потоком.
9. Обмен `rank` и `next_rank` через `vector::swap` без копирования.
10. Проверку условия остановки.

При чтении дополнительно проверяется:

- ребро принадлежит destination range текущего шарда;
- source находится в диапазоне вершин;
- source имеет ненулевую outdegree;
- записи строго отсортированы;
- фактически прочитанное число рёбер совпало с manifest.

После последней итерации программа требует:

```text
isfinite(rank_sum)
abs(rank_sum - 1.0) <= 1e-10
```

CSV записывается сначала во временный `OUTPUT.tmp`, а затем атомарно
переименовывается. Существующий output не перезаписывается.

## 10. Многопоточность и детерминизм

Шарды выдаются workers через атомарный счётчик следующей задачи. Это динамическое
расписание: поток, закончивший один файл, сразу берёт следующий.

Для каждого шарда выполняется правило single writer:

```text
worker A -> next_rank[shard A vertex range]
worker B -> next_rank[shard B vertex range]
```

Диапазоны не пересекаются, поэтому data race отсутствует. Массивы `rank` и
`outdegree` во время итерации доступны только для чтения.

Детерминизм обеспечивается несколькими решениями:

- рёбра одной destination отсортированы по source;
- одна destination обрабатывается ровно одним потоком;
- порядок сложения вкладов одной вершины не зависит от scheduler;
- локальные residual и rank sum хранятся по номеру шарда;
- главный поток сводит их в фиксированном порядке;
- для глобальных сумм используется compensated summation.

Автоматический тест требует побайтово одинаковый output CSV при запуске одного
графа с одним и четырьмя потоками.

### Что означает `--threads` в двух командах

В `pagerank` параметр задаёт реальное число worker threads. Фактически создаётся:

```text
min(requested_threads, shard_count)
```

В `preprocess` генерация sorted runs и merge в текущей версии однопоточные.
Параметр `--threads` используется при вычислении значения `--shards` по
умолчанию: `4 * threads`. Если `--shards` указан явно, `--threads` не ускоряет
preprocessing.

Это текущее ограничение реализации, а не скрытая параллельность.

## 11. Управление памятью

### PageRank

Основное resident-состояние:

| Структура | Размер |
|---|---:|
| `rank` | `8 * V` |
| `next_rank` | `8 * V` |
| `outdegree` | `4 * V` |
| Итого vertex state | `20 * V` байт |

Дополнительно учитываются:

- 128 KiB edge buffer на worker;
- резерв 8 MiB на runtime, stream objects, metadata и output buffers.

Preflight-оценка имеет вид:

```text
required = 20 * V + 128 KiB * workers + 8 MiB
```

Если required превышает `--memory-mb`, PageRank завершается до выделения больших
массивов с понятным сообщением.

Пример приблизительного верхнего числа вершин при 128 MiB:

```text
(128 MiB - runtime/thread reserve) / 20 bytes
```

Это примерно 5–6 миллионов вершин в зависимости от числа workers.

### Preprocessing

Для run generation используется около 80% указанного budget. Оставшиеся 20%
предназначены для executable, allocator metadata, файловых потоков и временных
объектов сортировки.

Merge ограничивает fan-in 64 файлами. Поэтому размер reader buffers не зависит
от полного числа runs. Для sparse ID mapping программа также заранее проверяет,
что `vertices.bin` помещается в рабочую часть budget.

### `--memory-mb` и реальный cgroup limit

`--memory-mb` — внутренний budget программы. Он контролирует управляемые
выделения, но не может полностью запретить Linux использовать свободную память
под page cache и read-ahead.

Строгую проверку всего процесса вместе с начисленным файловым cache нужно делать
через:

- systemd cgroup `MemoryMax`;
- или Docker `--memory` и `--memory-swap`.

В проекте LiveJournal успешно прошёл двухитерационный запуск при `MemoryMax=128M`
и `MemorySwapMax=0`.

## 12. Гипер-узлы

Ни preprocessing, ни PageRank не создают adjacency vector отдельной вершины.
Рёбра читаются ограниченными блоками, поэтому вершина степени 50 000 или больше
не приводит к аналогичному выделению памяти.

В тестах присутствует вершина с 50 000 входящих и 50 000 исходящих рёбер.

Есть различие между двумя типами гипер-узлов:

- source-гипер-узел встречается во многих destination ranges; его `rank` и
  `outdegree` просто читаются workers;
- destination-гипер-узел должен целиком остаться в одном шарде, чтобы сохранить
  single-writer ownership.

Экстремальный destination-гипер-узел остаётся memory-safe, но может стать
parallel straggler: один worker будет обрабатывать существенно больше рёбер,
чем остальные.

## 13. Быстрый старт

### 13.1. Сборка на NixOS

Все dev-зависимости находятся в `shell.nix`:

```bash
nix-shell
cmake -S . -B build-release \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOMG_BUILD_TESTS=OFF
cmake --build build-release
```

Полученный executable:

```text
build-release/omg
```

Проверка CLI:

```bash
./build-release/omg --help
```

### 13.2. Преобразование `cit-Patents`

Целевой каталог не должен существовать до запуска:

```bash
mkdir -p graphs

./build-release/omg preprocess \
  --input cit-Patents.txt \
  --graph-dir graphs/cit-patents.omg \
  --work-dir graphs/cit-patents.work \
  --memory-mb 128 \
  --threads 16 \
  --shards 64
```

Если `graphs/cit-patents.omg` уже существует, выберите новое имя. Программа
намеренно не перезаписывает готовый граф.

### 13.3. PageRank до сходимости

Родительский каталог output должен существовать, а сам файл — отсутствовать:

```bash
./build-release/omg pagerank \
  --graph-dir graphs/cit-patents.omg \
  --output graphs/cit-patents-pagerank.csv \
  --memory-mb 128 \
  --threads 16 \
  --damping 0.85 \
  --tolerance 1e-8 \
  --max-iterations 100
```

### 13.4. PageRank с фиксированным числом итераций

```bash
./build-release/omg pagerank \
  --graph-dir graphs/cit-patents.omg \
  --output graphs/cit-patents-pr-20.csv \
  --memory-mb 128 \
  --threads 16 \
  --damping 0.85 \
  --iterations 20
```

Если указан `--iterations`, параметры `--tolerance` и `--max-iterations` не
управляют остановкой.

### 13.5. Формат результата

CSV отсортирован по исходному vertex ID, а не по rank:

```csv
vertex,rank
1,0.000000123456789
2,0.000000987654321
```

Rank сериализуется с `max_digits10`, то есть с точностью, достаточной для
восстановления исходного `double`.

## 14. Полный справочник CLI

Общий синтаксис использует строгие пары `--option VALUE`. Неизвестные,
повторяющиеся и неполные параметры отклоняются.

### `omg preprocess`

```text
omg preprocess --input FILE --graph-dir DIR [options]
```

| Параметр | Обязательный | Default | Назначение |
|---|---:|---:|---|
| `--input FILE` | да | — | CSV или SNAP edge-list. |
| `--graph-dir DIR` | да | — | Новый каталог внутреннего графа. |
| `--work-dir DIR` | нет | `GRAPH_DIR.work` | Каталог временных sorted runs. |
| `--memory-mb N` | нет | `128` | Budget preprocessing; минимум 32 MiB. |
| `--threads N` | нет | hardware concurrency | Основа default shard count; preprocessing пока однопоточный. |
| `--shards N` | нет | `4 * threads` | Желаемое число destination-шардов. |

Рекомендации:

- `work-dir` размещать на SSD с достаточным свободным местом;
- для 16 hardware threads начинать с 64 шардов;
- не делать `work-dir` дочерним каталогом `graph-dir`;
- не изменять input во время двух проходов;
- заранее оставить временное место порядка нескольких размеров input.

Requested число шардов не обязательно будет достигнуто. Граница не может
разделить рёбра одной destination, а пустые шарды не создаются.

### `omg pagerank`

```text
omg pagerank --graph-dir DIR --output FILE [options]
```

| Параметр | Обязательный | Default | Назначение |
|---|---:|---:|---|
| `--graph-dir DIR` | да | — | Каталог, созданный `preprocess`. |
| `--output FILE` | да | — | Новый CSV-файл результата. |
| `--memory-mb N` | нет | `128` | Budget resident state и thread buffers. |
| `--threads N` | нет | hardware concurrency | Максимальное число worker threads. |
| `--damping X` | нет | `0.85` | Число строго между 0 и 1. |
| `--iterations N` | нет | — | Выполнить ровно `N > 0` итераций. |
| `--tolerance X` | нет | `1e-8` | Global L1 threshold для convergence mode. |
| `--max-iterations N` | нет | `100` | Лимит convergence mode. |

Если `--iterations` отсутствует, используется convergence mode.

### Коды завершения и потоки вывода

| Код | Значение |
|---:|---|
| `0` | Успех или явный `--help`. |
| `1` | Запуск без команды. |
| `2` | Ошибка параметров, данных, памяти или файловой системы. |

Progress и iteration statistics пишутся в `stderr`. Краткий финальный результат
PageRank пишется в `stdout`:

```text
iterations=20 residual_l1=5.8122e-09 rank_sum=1
```

## 15. Проверка результатов

### 15.1. Streaming validation CSV

Скрипт не загружает output целиком:

```bash
python scripts/validate_output.py graphs/cit-patents-pagerank.csv
```

Проверяется:

- заголовок `vertex,rank`;
- строгая сортировка vertex IDs;
- конечность и неотрицательность рангов;
- сумма рангов с Kahan summation;
- отсутствие пустого результата.

Пример JSON-ответа:

```json
{
  "maximum_absolute_error": null,
  "maximum_rank": 0.0001,
  "maximum_relative_error": null,
  "minimum_rank": 1e-9,
  "rank_sum": 1.0,
  "vertices": 3774768
}
```

### 15.2. Сравнение с эталоном

```bash
python scripts/validate_output.py \
  actual.csv \
  --expected reference.txt \
  --relative-tolerance 1e-4
```

Скрипт требует совпадение IDs и вычисляет maximum absolute/relative error.

### 15.3. Graphalytics fixture

В репозитории находится официальный directed PageRank fixture:

```text
tests/data/graphalytics/test-pr-directed.e
tests/data/graphalytics/test-pr-directed-PR
```

Параметры reference:

- 50 вершин;
- 246 рёбер;
- damping `0.85`;
- 14 итераций;
- допустимая относительная ошибка `1e-4`.

Полученная максимальная относительная ошибка:

```text
1.2677113294486482e-06
```

### 15.4. Проверка checksum датасетов

Использованные во время разработки файлы:

```text
d2a11214ec084e3767f147e8736fc9f723aea5f1b067632007d5206cf54c8529  cit-Patents.txt
4bfdd9975179fbf180619b99989bc9f4a25e9371a6c8c853f1c5c611240c071b  soc-LiveJournal1.txt
```

Проверка:

```bash
sha256sum cit-Patents.txt soc-LiveJournal1.txt
```

Это исключает случайное сравнение результатов на разных версиях входа.

## 16. Тесты и quality gate

### 16.1. Обычная тестовая сборка

```bash
nix-shell
cmake -S . -B build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DOMG_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### 16.2. ASan и UBSan

```bash
cmake -S . -B build-sanitize \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DOMG_BUILD_TESTS=ON \
  -DOMG_ENABLE_SANITIZERS=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

### 16.3. Что покрывают C++-тесты

Текущий набор содержит десять тестовых сценариев:

- CSV header и signed `int32` IDs;
- SNAP comments и whitespace-разделители;
- сообщение с номером строки для malformed input;
- round-trip manifest JSON;
- sparse/negative ID remapping;
- удаление duplicate edges;
- построение destination-шардов;
- аналитическое решение графа с dangling vertex;
- детерминизм между одним и четырьмя потоками;
- официальный Graphalytics reference;
- гипер-узел степени 50 000;
- запрет `work-dir` внутри `graph-dir`.

Некоторые свойства проверяются в одном общем сценарии, поэтому список свойств
длиннее числа Catch2 test cases.

### 16.4. Форматирование и статический анализ

```bash
clang-format --dry-run --Werror src/*.cpp include/omg/*.hpp tests/*.cpp
ruff format --check scripts
ruff check scripts
shellcheck scripts/*.sh
clang-tidy -p build \
  src/input.cpp \
  src/main.cpp \
  src/manifest.cpp \
  src/pagerank.cpp \
  src/preprocess.cpp
```

Финальный quality gate прошёл в Debug, Release, ASan/UBSan и `clang-tidy` без
диагностик.

## 17. Бенчмарки и фактические результаты

### 17.1. Стенд

```text
OS: Linux x86-64, little-endian
CPU: AMD Ryzen 7 8845H
Cores/threads: 8/16
NUMA nodes: 1
L3: 16 MiB
Storage: NVMe SSD
Development compiler: GCC 15.2.0
Ubuntu 22.04 Docker compiler: GCC 11.4.0
```

Один NUMA node означает, что для этого стенда не требуется NUMA-aware
репликация rank arrays.

### 17.2. Датасеты

| Dataset | Вершины | Уникальные рёбра | Текстовый input |
|---|---:|---:|---:|
| `cit-Patents` | 3 774 768 | 16 518 948 | 280 538 727 B |
| `soc-LiveJournal1` | 4 847 571 | 68 993 773 | 1 080 598 042 B |

Только binary edge set LiveJournal занимает около 526 MiB, то есть заведомо не
помещается в лимит 128 MiB.

### 17.3. Preprocessing

Параметры: 128 MiB budget и 64 requested shards.

| Dataset | Wall time | Max RSS | Vertex runs | Edge runs |
|---|---:|---:|---:|---:|
| `cit-Patents` | 10.58 s | 108 012 KiB | 2 | 2 |
| `soc-LiveJournal1` | 43.80 s | 108 344 KiB | 6 | 7 |

В этих двух inputs duplicate edges не обнаружены. Найдены:

- один self-loop в `cit-Patents`;
- 518 382 self-loop в `soc-LiveJournal1`.

### 17.4. PageRank до `L1 <= 1e-8`

Параметры: 16 threads, damping `0.85`.

| Dataset | Итерации | Финальный residual | Wall time с CSV | Max RSS |
|---|---:|---:|---:|---:|
| `cit-Patents` | 20 | `5.8122e-9` | 1.13 s | 80 784 KiB |
| `soc-LiveJournal1` | 78 | `9.0892e-9` | 22.33 s | 102 280 KiB |

Измерения выполнялись с тёплым filesystem cache. Их нельзя выдавать за cold SSD
throughput без отдельного protocol очистки cache.

### 17.5. Масштабирование по потокам

`cit-Patents`, пять итераций, warm cache:

| Threads | Median iteration | Median edges/s | Speedup |
|---:|---:|---:|---:|
| 1 | 0.2932 s | 56.34 M | 1.00x |
| 2 | 0.1443 s | 114.45 M | 2.03x |
| 4 | 0.0751 s | 220.07 M | 3.91x |
| 8 | 0.0461 s | 358.29 M | 6.36x |
| 16 | 0.0400 s | 412.55 M | 7.32x |

После восьми потоков прирост уменьшается из-за SMT, общей memory bandwidth и
конкуренции за filesystem cache.

Повторить benchmark:

```bash
python scripts/benchmark_threads.py \
  --executable build-release/omg \
  --graph-dir graphs/cit-patents.omg \
  --threads 1 2 4 8 16 \
  --iterations 5 \
  --repetitions 3 \
  --memory-mb 128
```

Скрипт удаляет создаваемые им временные CSV после каждого запуска и печатает
JSON со всеми измерениями и медианами.

### 17.6. Жёсткий memory limit

Готовый helper:

```bash
scripts/check_memory.sh \
  ./build-release/omg \
  graphs/livejournal.omg \
  graphs/livejournal-memory-check.csv \
  16 \
  2
```

Он использует:

```text
MemoryMax=128M
MemorySwapMax=0
```

Измеренный cgroup peak для LiveJournal составил 127.9 MiB, swap — 0 B. Peak
включает начисленные процессу page-cache pages.

## 18. Структура исходного кода

### Корень проекта

| Файл | Назначение |
|---|---|
| `CMakeLists.txt` | C++20 targets, warnings, sanitizers и Catch2 tests. |
| `shell.nix` | GCC, CMake, Ninja, Catch2, JSON, Python, анализаторы и профилировщики. |
| `Dockerfile` | Multi-stage Ubuntu 22.04 release build. |
| `README.md` | Краткая инструкция для проверяющего. |
| `RESEARCH.md` | Исследование метрик, датасетов и архитектур. |

### Production code

| Файл | Ответственность |
|---|---|
| `src/main.cpp` | CLI parser, defaults, строгая проверка опций, dispatch команд. |
| `src/input.cpp` | Потоковый CSV/SNAP parser. |
| `src/manifest.cpp` | JSON serialization и проверка файлов графа. |
| `src/preprocess.cpp` | External sort, dense remapping, deduplication и sharding. |
| `src/pagerank.cpp` | Memory preflight, parallel pull iterations и CSV output. |
| `include/omg/types.hpp` | Типы ID, edge records и platform assertions. |
| `include/omg/input.hpp` | Интерфейс streaming reader. |
| `include/omg/manifest.hpp` | Manifest structures и format constants. |
| `include/omg/preprocess.hpp` | `PreprocessOptions` и API preprocessing. |
| `include/omg/pagerank.hpp` | `PageRankOptions`, result и API алгоритма. |

### Tests и fixtures

| Файл | Назначение |
|---|---|
| `tests/input_test.cpp` | Форматы input и malformed lines. |
| `tests/manifest_test.cpp` | JSON round-trip. |
| `tests/preprocess_test.cpp` | Mapping, deduplication, shards и безопасные пути. |
| `tests/pagerank_test.cpp` | Dangling case, determinism, Graphalytics и hyper-node. |
| `tests/data/graphalytics/` | Официальный небольшой reference graph. |

### Scripts

| Файл | Назначение |
|---|---|
| `scripts/validate_output.py` | Streaming validation и сравнение с reference. |
| `scripts/benchmark_threads.py` | Повторяемое измерение iteration throughput. |
| `scripts/check_memory.sh` | Запуск PageRank в systemd cgroup 128 MiB. |

## 19. Ошибки и диагностика

### `graph directory already exists`

`preprocess` не перезаписывает существующий граф. Выберите новый `--graph-dir`.
Такое поведение защищает готовый результат от случайной потери.

### `output file already exists`

`pagerank` также не перезаписывает output. Используйте новое имя или осознанно
переместите старый результат перед повторным запуском.

### `output parent directory does not exist`

Создайте родительский каталог заранее:

```bash
mkdir -p results
```

### `PageRank requires approximately ... MiB`

Vertex state не помещается в заданный budget. Возможные действия:

- увеличить `--memory-mb`, если это разрешено;
- уменьшить `--threads`, чтобы сократить thread buffers;
- перейти к partitioned-state/2D tiled архитектуре для существенно большего `V`.

Уменьшение числа шардов не уменьшает два rank arrays и outdegree array.

### `PageRank did not converge within ... iterations`

`--max-iterations` оказался слишком мал для заданного tolerance. Увеличьте лимит
или осознанно ослабьте tolerance. Не следует автоматически объявлять последний
вектор сошедшимся.

### `invalid edge at FILE:LINE`

Строка не соответствует поддерживаемому формату или ID не помещается в signed
`int32`. Поддерживаются ровно два числовых поля, CSV comma или whitespace.

### `work directory must not be the graph directory or its child`

Перенесите временные runs за пределы будущего каталога графа, например:

```text
--graph-dir graphs/example.omg
--work-dir work/example
```

### Процесс завершён cgroup OOM

Внутренний budget не контролирует все страницы filesystem cache. Проверьте:

- совпадает ли `--memory-mb` с cgroup `MemoryMax`;
- можно ли уменьшить threads;
- не слишком ли близок application estimate к hard limit;
- отключён ли swap именно так, как требует эксперимент.

### Второй запуск оказался намного быстрее

Вероятная причина — тёплый filesystem cache. Это нормально. Для сравнения
алгоритмов нужно использовать единый cold/warm protocol и несколько повторов.

## 20. Ограничения

Текущая версия имеет следующие осознанные ограничения:

1. Vertex state размера примерно `20 * V` должен помещаться в RAM.
2. Version 1 работает только на little-endian x86-64.
3. Граф статичен после preprocessing.
4. Edge-only input не позволяет узнать об изолированных вершинах.
5. Пустой граф без рёбер не поддерживается.
6. Генерация sorted runs и merge однопоточные; многопоточной является основная
   итерационная фаза PageRank.
7. Один destination-гипер-узел не делится между workers и может стать straggler.
8. Рёбра во внутреннем формате не сжаты.
9. Очень большое число runs приводит к дополнительным merge-проходам и SSD I/O,
   хотя память и число открытых файлов остаются ограниченными.
10. Convergence tolerance измеряет разность соседних итераций, а не напрямую
    расстояние до стационарного вектора.
11. Исходный файл должен оставаться неизменным между двумя проходами
    preprocessing.
12. Output отсортирован по vertex ID; поиск top-N по rank — отдельная операция.

Эти ограничения не мешают обработке двух выбранных реальных датасетов при
заданном лимите 128 MiB.

## 21. Что можно улучшить

Наиболее полезные следующие шаги:

1. Параллельная генерация sorted runs с отдельными bounded worker buffers.
2. Явная проверка неизменности input между двумя проходами по размеру, mtime или
   checksum/edge count.
3. Special chunks и deterministic partial reduction для экстремальных
   destination-гипер-узлов.
4. Optional vertex-list для изолированных вершин.
5. Partitioned rank state или 2D tiles для графов, где даже `20 * V` не
   помещается в RAM.
6. Block compression, например delta coding source IDs внутри destination
   blocks.
7. Checkpoints между итерациями для восстановления после сбоя.
8. Cold-cache benchmark protocol и более длинные статистические серии.
9. Предвычисленные reciprocal outdegrees, если memory budget позволяет заменить
   integer division умножением.
10. Переносимый version 2 формата с явной endian conversion.

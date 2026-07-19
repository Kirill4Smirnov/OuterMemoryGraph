# PageRank для графа во внешней памяти

Программа вычисляет PageRank ориентированного графа на одном компьютере с ограниченным объёмом оперативной памяти

Список рёбер хранится на твердотельном накопителе в нескольких файлах, а в оперативной памяти остаются только ранги и исходящие степени вершин

На одну вершину требуется около 20 байт оперативной памяти

## Возможности

- чтение CSV и списков рёбер SNAP без загрузки всего файла в память
- внешняя сортировка с заданным ограничением памяти
- преобразование произвольных `int32` идентификаторов во внутренние последовательные номера
- удаление повторных рёбер и сохранение петель
- многопоточная сортировка временных файлов
- многопоточный PageRank без конфликтующих записей
- перераспределение ранга вершин без исходящих рёбер
- потоковая обработка вершин степени 50 000 и выше
- воспроизводимый результат при разном порядке работы потоков

## Требования

- Ubuntu 22.04 x64
- GCC 11 или новее
- CMake 3.22 или новее
- твердотельный накопитель с местом для исходного файла, временных данных и подготовленного графа

## Сборка на Ubuntu 22.04

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  nlohmann-json3-dev \
  python3 \
  ca-certificates \
  wget \
  gzip \
  unzip

cmake -S . -B build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOMG_BUILD_TESTS=OFF
cmake --build build
```

Готовая программа находится в `build/omg`

```bash
./build/omg --help
```

## Входные данные

CSV из двух столбцов

```csv
from,to
1,2
2,3
3,1
4,1
```

Поддерживается и обычный формат SNAP с пробелом или табуляцией

```text
# FromNodeId ToNodeId
1 2
2 3
```

Идентификаторы вершин должны помещаться в знаковый `int32`

Множество вершин определяется по концам рёбер, поэтому изолированные вершины задать нельзя

## Запуск

Расчёт состоит из подготовки графа и итераций PageRank

### 1 Подготовка графа

```bash
./build/omg preprocess \
  --input edges.csv \
  --graph-dir data/graph.omg \
  --work-dir data/work \
  --memory-mb 128 \
  --threads 8 \
  --shards 32
```

Исходный файл читается два раза

Между проходами сверяются число рёбер и их контрольная сумма

Каталог `graph-dir` не должен существовать заранее, а `work-dir` не должен находиться внутри него

### 2 Вычисление PageRank

Остановка при L1-разности соседних векторов не больше `1e-8`

```bash
./build/omg pagerank \
  --graph-dir data/graph.omg \
  --output pagerank.csv \
  --memory-mb 128 \
  --threads 8 \
  --damping 0.85 \
  --tolerance 1e-8 \
  --max-iterations 100
```

Запуск ровно на 20 итераций

```bash
./build/omg pagerank \
  --graph-dir data/graph.omg \
  --output pagerank-20.csv \
  --memory-mb 128 \
  --threads 8 \
  --damping 0.85 \
  --iterations 20
```

Результат отсортирован по исходным идентификаторам

```csv
vertex,rank
1,0.35
2,0.25
```

Существующий файл результата программа не перезаписывает

Параметр `--memory-mb` используется для предварительной проверки выделений памяти, но не заменяет системное ограничение

Для жёсткого ограничения памяти нужен контейнер Docker или контрольная группа Linux

## Проверка на эталонном графе

В репозитории лежит небольшой тестовый граф LDBC Graphalytics с известным результатом

```bash
./build/omg preprocess \
  --input tests/data/graphalytics/test-pr-directed.e \
  --graph-dir /tmp/graphalytics.omg \
  --work-dir /tmp/graphalytics-work \
  --memory-mb 128 \
  --threads 4 \
  --shards 8

./build/omg pagerank \
  --graph-dir /tmp/graphalytics.omg \
  --output /tmp/graphalytics-pagerank.csv \
  --memory-mb 128 \
  --threads 4 \
  --damping 0.85 \
  --iterations 14

python3 scripts/validate_output.py \
  /tmp/graphalytics-pagerank.csv \
  --expected tests/data/graphalytics/test-pr-directed-PR
```

## Большие наборы данных

Для проверки использовались `cit-Patents` и `soc-LiveJournal1` из коллекции SNAP

Рабочие копии этих наборов есть на Kaggle

```bash
wget -O cit-Patents.zip \
  https://www.kaggle.com/api/v1/datasets/download/wolfram77/graph-snap-cit-patents
wget -O soc-LiveJournal1.zip \
  https://www.kaggle.com/api/v1/datasets/download/lohithkandibanda/soc-livejournal1-txt-gz

unzip cit-Patents.zip cit-Patents.txt
unzip soc-LiveJournal1.zip soc-LiveJournal1.txt
```

Эти файлы и полученные из них графы не нужно добавлять в Git

## Docker

```bash
docker build -t outer-memory-graph .
docker run --rm --memory=128m --memory-swap=128m \
  -v "$PWD:/data" \
  outer-memory-graph --help
```

## Ограничения

- массивы состояния размером около `20 * V` байт должны помещаться в оперативную память
- внутренний формат рассчитан на Linux x64 с порядком байтов от младшего к старшему
- подготовленный граф нельзя изменять
- один очень крупный узел с высокой входящей степенью может задержать завершение одного потока
- параметр `--tolerance` задаёт разность соседних векторов, а не точную ошибку относительно стационарного решения

## Документация

- [Отчёт по решению](docs/report.md)
- [Алгоритм и оценки](docs/solution.md)
- [Способ хранения и альтернативы](docs/alternatives.md)
- [Проверка корректности и памяти](docs/verification.md)
- [Форматы данных](docs/formats.md)
- [Внутренний формат графа](docs/file-format.md)

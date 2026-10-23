# wheeliestor

Стенд для сравнения libc и io_uring как механизмов ввода-вывода
клиент-серверного приложения, которое передаёт данные по TCP и хранит их на
диске. Подробности в [docs/design.md](docs/design.md).

## Сборка

```
git submodule update --init --depth 1
./autogen.sh
./configure --prefix=$PWD/.install
make -j$(nproc)
make test
```

Стенд из двух контейнеров и скрипт замеров `bench/run.sh` описаны в
[docs/bench.md](docs/bench.md); как мерить на новом железе, читать результаты
и искать, где потеряна скорость, — в [docs/methodology.md](docs/methodology.md).

Движок для fio (`fio/.libs/wheelie.so`) собирается против исходников fio из
подмодуля `3rdparty/fio`; подробности в [docs/fio.md](docs/fio.md).

Нужны компилятор C/C++20, autotools, libtool, pkg-config, liburing >= 2.3 и
GoogleTest.

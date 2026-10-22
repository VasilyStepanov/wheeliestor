# wheeliestor

Стенд для воркшопа: сравнение libc и io_uring как механизмов ввода-вывода
клиент-серверного приложения, которое передаёт данные по TCP и хранит их на
диске. Подробности в [docs/design.md](docs/design.md).

## Сборка

```
./autogen.sh
./configure --prefix=$PWD/.install
make -j$(nproc)
make test
```

Нужны компилятор C/C++20, autotools, libtool, pkg-config, liburing >= 2.3 и
GoogleTest.

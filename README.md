# Glos
Compiler for the Glos programming language.

## Quick Start
```console
$ cc -o first first.c # On Linux/macOS
$ cl first.c          # On Windows
$ ./first
```

> [!NOTE]
> You don't need to recompile `first.c` after modifying it. You can just run `./first`, and it will recompile itself if needed.

## Hello, world!
```
// sample.glos
main :: () {
    println("Hello, world!")
}
```

Compile and run it like this:

```console
$ glos sample.glos
$ ./sample
Hello, world!
```

## Tutorial
See the files in [how_to](how_to).

## Tests
```console
$ ./first -t # Interactive mode
$ ./first -T # Non-interactive mode
```

## Platforms supported
- x86_64 Linux
- x86_64 Windows
- ARM64 macOS

# Glos

> [!WARNING]
> This repo is WIP.

## Quick Start
```console
$ make
```

## Examples
### Hello, world!
```go
package main

import "fmt"

fn main() {
    fmt::println("Hello, world!")
}
```

### Dynamic Arrays
```go
package main

import "fmt"

fn main() {
	var xs [..]i64
	for var i = 0; i < 10; i += 1 {
		xs.push(i * 2)
	}

	for var i = 0; i < len(xs); i += 1 {
		fmt::println(xs[i])
	}
}
```

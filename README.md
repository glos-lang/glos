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

import "io"

fn main() {
    io::write("Hello, world!\n")
}
```

### Dynamic Arrays
```go
package main

import "da"

fn main() {
	var xs [..]i64
	for var i = 0; i < 10; i += 1 {
		da::push(&xs, i * 2)
	}

	for var i = 0; i < len(xs); i += 1 {
		print xs[i]
	}
}
```

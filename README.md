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

import "array"

fn main() {
	var xs array::Array<i64>
	for var i = 0; i < 10; i += 1 {
		xs.push(i * 2)
	}

	for var i = 0; i < len(xs.items); i += 1 {
		print xs.items[i]
	}
}
```

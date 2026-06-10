<img src="docs/images/screenshot1.jpg" alt="ServoQ overview" width="800">

# ServoQ

Ladybird's Qt6 browser chrome, running on Servo instead of LibWeb.

## Background

I've been following both [Ladybird](https://github.com/LadybirdBrowser/ladybird) and
[Servo](https://servo.org/) for a while. Ladybird recently got a clean Qt6 UI rewrite,
and separately I came across [this KDAB blog post](https://www.kdab.com/embedding-servo-in-qt/)
from early 2024, a basic proof-of-concept of embedding Servo in a Qt app using CXX-Qt.
That demo was minimal and never continued.

Which raised a question: could you take Ladybird's actual Qt frontend and wire
it up to Servo instead of LibWeb? It turns out you can, with some changes to make it
possible. That's ServoQ.

The C++ chrome under `cpp/` is ported from Ladybird's Qt UI and several of its support
libraries (search engine handling, bookmark storage, history, content blocking). Each
file's header identifies which Ladybird source it was derived from. The Rust side under
`src/` handles Servo embedding. Going forward I'll probably diverge from Ladybird's UI
more and more, but I also intend to keep pulling in improvements from upstream where they
make sense.

# Feature status

## Missing browser features (WIP)

* Downloads UI
* Search suggestions from search engines
* Automatic filter-list updates
* Per-site zoom settings
* DevTools
* and much more...

### Features currently blocked by Servo

* Selecting Text on Websites
* Scroll Bars
* Find in page, including match count
* Printing pages
* Tab audio indicator
* Cosmetic content blocking

## A note on how this was built

I'm a computer science student from Germany. I know my way around code, but I'm not
particularly experienced, and systems-level C++/Rust is well outside my usual territory.
This was a personal experiment: could I actually pull this off, leaning on AI tools 
for the parts I'd otherwise get stuck on? Turns out, mostly yes.

Worth being upfront about: both Ladybird and Servo are not to keen on AI-generated
contributions to their own codebases. ServoQ is an independent hobbyist project and does
not contribute back to either upstream.

## Building

**Dependencies** :

```
rust cargo base-devel gcc qt6-base qt6-svg qt6-networkauth qt6-wayland pkg-config qt6-tools clang
```

```sh
git clone https://github.com/herzblutnord/servoq
cd servoq
cargo build --release --features servo-engine
cargo run --release --features servo-engine
```

## Attribution

**[Ladybird Browser](https://github.com/LadybirdBrowser/ladybird)** - The Qt UI code and
supporting library code in `cpp/` is ported from Ladybird, developed by the Ladybird
Browser Initiative and contributors. Licensed under BSD 2-Clause. Original copyright
notices are preserved in each derived source file.

**[Servo](https://github.com/servo/servo)** - The rendering engine, used as a Rust crate
dependency. Originally created by Mozilla Research, now maintained under Linux Foundation
Europe. Licensed under MPL 2.0.

**[KDAB cxx-qt-servo-webview](https://github.com/KDABLabs/cxx-qt-servo-webview)** - The
earlier Qt/Servo experiment that started this idea.

## License

BSD 2-Clause. See [LICENSE](LICENSE). Servo is a dependency and remains under its own
MPL 2.0 license.
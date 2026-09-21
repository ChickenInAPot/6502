# Project notes

## Timeline

- **2024:** early EEPROM programming experiments, preserved in the existing Git history.
- **July 2026:** Mega-based programming/debugging work, the host GUI/CLI, and counting-ROM work. The local project files and July work history are the source for this period.
- **September 2026:** collected the current source into the existing GitHub repository and added setup and validation notes.

The publication date is separate from when the hardware work happened. Earlier commits have not been rewritten.

## Design choices

The direct-bus Mega version can work as a standalone programmer or share the breadboard computer's bus. The separate shift-register version reduces the number of address wires connected directly to the Mega. Both compare existing EEPROM contents before writing, which avoids rewriting an unchanged image.

The Python tool provides a terminal workflow as well as a Tk interface. The GUI limits the amount of trace output it processes, so a busy serial link does not grow the console indefinitely.

## References and older files

- Ben Eater's [6502 breadboard series](https://eater.net/6502) is the hardware reference.
- The local archive also contains a `code.s` LCD example credited in its header to **Andrew Miller (2020)**. It is reference material and is not presented here as original work.
- An older Arduino bus harness was found locally. Its wiring differs from the current implementation, so it has not been mixed into the current build.
- Earlier repository branches remain available in GitHub history. The two folders linked in the main README are the current documented starting points.

No blanket license is added to reference code or dependencies. Their existing licenses and attribution remain applicable.

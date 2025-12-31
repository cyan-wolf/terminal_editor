# Terminal Editor
This is a terminal editor written in C.

## Installation
The editor is meant for Unix systems such as MacOS and Linux. 

On systems with the `apt` package manager, the project can be built as follows:

```bash
sudo apt install gcc make
make build
make run
```

## Usage
Running the `make build` command creates an executable called `text-editor`. When this executable is called with no arguments, the editor is just a text buffer with no associated file. To associate the editor with a file press the CTRL-S command, this prompts the user for a file name. 

When the executable is called with a file name as a command line argument, the editor opens this file and allows editing its content. The editor provides basic syntax highlighting for C and C++ files. 

### Commands
* CTRL-S: Saving a new file or for modifying an existing file.
* CTRL-F: For searching for a particular substring.
* CTRL-Q: Exits the editor. When the editor detects unsaved changes, the user must press this command three times to exit without saving.

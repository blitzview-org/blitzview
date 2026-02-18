# ⚡ BlitzView

> A fast desktop image browser and manager with EXIF tag support

BlitzView is a lightweight, cross-platform image viewer built with Qt6 that focuses on speed and simplicity. Browse your image collections efficiently with a clean interface and powerful features.

## Features

### Current Features
- 🖼️ **Fast Image Viewing** - Quick image loading and display
- 📁 **Folder Navigation** - Browse images with an integrated file tree
- 🎨 **Multiple Format Support** - JPG, PNG, BMP, GIF, TIFF, WebP
- 📊 **Image Information** - View filename, dimensions, and file size in the status bar
- ⚡ **Responsive UI** - Resizable sidebar and image view with QSplitter

### Planned Features
- 📷 **EXIF Tag Viewing** - Display image metadata and camera settings
- ✏️ **EXIF Tag Editing** - Modify image metadata
- 🚀 **Fast Thumbnail Browsing** - Quick preview of image collections
- 🗂️ **Image Management** - Organize, rename, and manage your images
- 🔍 **Advanced Search** - Find images by metadata, date, or tags

## Dependencies

- **Qt6 Widgets** (6.0 or later)
- **CMake** (3.16 or later)
- **C++17** compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)

## Building

### Linux (Primary Platform)

#### Ubuntu/Debian
```bash
# Install dependencies
sudo apt update
sudo apt install build-essential cmake qt6-base-dev

# Build
mkdir build && cd build
cmake ..
make

# Run
./src/blitzview
```

#### Fedora/RHEL
```bash
# Install dependencies
sudo dnf install gcc-c++ cmake qt6-qtbase-devel

# Build
mkdir build && cd build
cmake ..
make

# Run
./src/blitzview
```

#### Arch Linux
```bash
# Install dependencies
sudo pacman -S base-devel cmake qt6-base

# Build
mkdir build && cd build
cmake ..
make

# Run
./src/blitzview
```

### Windows

1. Install [Qt6](https://www.qt.io/download) (including Qt Widgets)
2. Install [CMake](https://cmake.org/download/)
3. Install a C++ compiler (Visual Studio 2017+ or MinGW)

```cmd
mkdir build && cd build
cmake -G "Visual Studio 17 2022" ..
cmake --build . --config Release

# Run
.\src\Release\blitzview.exe
```

### macOS

```bash
# Install dependencies (using Homebrew)
brew install cmake qt6

# Build
mkdir build && cd build
cmake ..
make

# Run
./src/blitzview
```

## Installation

```bash
# After building
cd build
sudo make install
```

## Usage

- **Open Image**: `File → Open Image` or `Ctrl+O`
- **Open Folder**: `File → Open Folder` or `Ctrl+Shift+O`
- **Navigate**: Click on images in the sidebar to view them
- **Resize**: Drag the splitter between the sidebar and image view

## License

BlitzView is licensed under the [MIT License](LICENSE).

Copyright © 2026 Oliver

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

## Roadmap

- [ ] EXIF metadata viewing
- [ ] EXIF metadata editing
- [ ] Thumbnail grid view
- [ ] Image slideshow mode
- [ ] Basic image transformations (rotate, flip)
- [ ] Keyboard navigation between images
- [ ] Zoom controls
- [ ] Recent files/folders
- [ ] Settings/preferences dialog

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

mx-repo-manager is a Qt6 desktop application for managing APT repositories on MX Linux and Debian systems. It provides a GUI for enabling/disabling repositories, finding fastest mirrors, and managing APT sources configuration files. The application requires administrative privileges and uses pkexec/gksu for elevation.

## Build System and Development Commands

### Building the Application
```bash
# Configure and build with CMake + Ninja
./build.sh

# Or manually:
# cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
# cmake --build build --parallel
```

### Translation Management
```bash
# Update translation files
/usr/lib/qt6/bin/lrelease translations/*.ts
```

### Debian Package Development
```bash
# Clean build artifacts
./build.sh --clean

# Build Debian package (uses debian/rules)
./build.sh --debian
```

## Code Architecture

### Core Components

- **MainWindow** (`mainwindow.cpp/h`): Primary UI controller managing repository display, user interactions, and configuration changes
- **Cmd** (`cmd.cpp/h`): Process execution wrapper handling both regular and elevated (root) command execution
- **About** (`about.cpp/h`): About dialog component

### Key Architecture Patterns

#### Privilege Elevation System
The application uses a two-tier privilege system:
- Normal user execution for UI and non-privileged operations
- Root elevation via pkexec/gksu for APT operations through `/usr/lib/mx-repo-manager/helper` script

#### Repository Management Flow
1. Parse APT source files from `/etc/apt/sources.list.d/`
2. Display repositories in categorized tree widget (MX repos, Debian repos, etc.)
3. Queue configuration changes during user interaction
4. Apply changes atomically when user confirms

#### Network Operations
Uses Qt's network classes for:
- Downloading MX repository lists
- Mirror speed testing for fastest mirror selection
- HTTP timeout handling (10s default)

### File Structure Patterns

- **UI Files**: `.ui` files for Qt Designer layouts
- **Translations**: `translations/` directory with `.ts` files for internationalization
- **Resources**: `images.qrc` for embedded icons and assets
- **Helper Scripts**: `scripts/` directory containing privilege elevation helper

## Development Notes

### Qt Configuration
- Uses Qt6 with widgets and network modules
- C++20 standard with strict compiler warnings
- Debug/release configuration support with LTO optimization
- Desktop integration via .desktop file and PolicyKit policy

### Security Considerations
- Root execution guard prevents running as root user
- Privilege elevation only for specific system operations
- Helper script validates and executes privileged commands safely

### Testing Repository Changes
After making changes to repository management logic:
1. Test with various APT source configurations
2. Verify privilege elevation works correctly
3. Test mirror selection and URL validation
4. Ensure backup/restore functionality works

### Version Management
Version information is generated from `debian/changelog` during build process and stored in `version.h`.
# 📦 Arma PAK Plugin for Total Commander

[![Version](https://img.shields.io/badge/version-1.2.0-blue.svg)](https://github.com/Rendszerguru/ArmaPAK-TC/releases/latest)
[![TC IrfanView](https://img.shields.io/badge/TC%20IrfanView-Plugin-orange.svg)](https://totalcmd.net/plugring/TCIrfanViewPlugin_2.0.html)
[![Plugman](https://img.shields.io/badge/Plugman-Compatible-success.svg)](https://totalcmd.net/plugring/tc_plugman.html)
[![License](https://img.shields.io/badge/license-Free-lightgrey.svg)](#-license)

This plugin allows Total Commander to handle Arma Reforger `.PAK` archives as if they were regular folders. You can browse, search, and extract files with specialized support for game assets.

<img width="512" height="512" alt="ArmaPAK_Logo" src="https://github.com/user-attachments/assets/9f6c5e7a-5c4d-42ed-84d0-38aee0a3d048" />

---

### ✨ **Key Features**

- **Workbench Integration:** 🛠 Open `.xob` models or other assets directly in the **Arma Reforger Workbench** with a single click from the extraction dialog. The plugin handles dependencies and workbench launching automatically.
- **Interactive Configuration:** ⚙️ Every archive contains a virtual `pak_plugin.ini` file. Simply press **F3 (Lister)** on it to instantly open the plugin's graphical settings panel without leaving the archive.
- **Quick Extraction Options (F5 / Alt+F9):** ⚡ A compact dialog appears before copying or unpacking, allowing you to modify **extraction settings** on-the-fly.
- **Smart Extract (Dependency Handling):** 🧠 When extracting models (`.xob`) or materials (`.emat`), the plugin automatically finds and extracts all required assets (like `.edds` textures) from the **currently active or opened archives**.
- **Intelligent Folders:** 📂 Optionally preserves original folder hierarchy and prevents redundant folder levels (e.g., `scripts/scripts/`) while ensuring seamless file viewing (**F3**) without extra directories.
- **High-Performance Engine:** 🚀 Custom **Multi-threaded ThreadPool** for parallel extraction and an **$O(1)$ PakIndex lookup system** (hash-map) for instant file access.
- **Full-text Search Support:** Use Total Commander’s **Find Files** (`Alt + F7`) with **Find text** enabled to search directly within archive contents.
- **Viewer Integration:** View game assets directly in the viewer (**F3**) using [IrfanView](https://www.irfanview.com) and the [TC IrfanView Plugin](https://totalcmd.net/plugring/TCIrfanViewPlugin_2.0.html).

---

### ⚙️ **Configuration**

Settings such as **Smart Extraction**, **Directory Structure**, and **Logging** can be managed via the built-in interactive dialog or by editing the `pak_plugin.ini` file.

- **Automated Logging:** A `pak_plugin.log` records critical errors with a built-in **5MB rotation limit**.
- **Resource Optimization:** Enhanced memory and GDI management ensures all UI assets and buffers are properly released.

---

### 🚀 **Installation**

#### **Automatic Installation**
Unzip the **ArmaPAK-TC.zip** and press **Enter** on the `.wcx` file inside Total Commander for automatic installation.

#### **Manual Installation**
1. Extract `ArmaPAK.wcx` (and/or `ArmaPAK.wcx64`) to your Total Commander `Plugins\wcx\` folder.
2. Go to **Configuration** → **Options** → **Plugins**.
3. Under **Compressor Plugins (.WCX)**, click **Configure**.
4. Type **PAK** in the extension box, click **New Type**, and select `ArmaPAK.wcx`.
5. Click **OK**.

---

### 📖 **Usage**
- **Browse:** Press **Enter** or **Ctrl+PageDown** on a `.pak` file.
- **Extract (F5 / Alt+F9):** Use **F5** (Copy) or **Alt+F9** (Unpack). Use the **"Open in Workbench"** button for rapid asset editing.
- **Quick Settings:** Locate the `pak_plugin.ini` inside any PAK and press **F3** to adjust plugin behavior instantly.
- **Search:** Press **Alt + F7**, enable **Find text**, and search within archives.

---

### 📄 **License**
This plugin is **free software**, released **"as is."** Redistribution is permitted as long as the distribution remains intact.

---

### 🧑‍💻 **Author**
**Icebird** Copyright © 2026. All rights reserved.

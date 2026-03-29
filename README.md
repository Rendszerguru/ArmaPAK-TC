# 📦 Arma PAK Plugin for Total Commander

[![Version](https://img.shields.io/badge/version-1.1.5-blue.svg)](https://github.com/Rendszerguru/ArmaPAK-TC/releases/latest)
[![TC IrfanView](https://img.shields.io/badge/TC%20IrfanView-Plugin-orange.svg)](https://totalcmd.net/plugring/TCIrfanViewPlugin_2.0.html)
[![Plugman](https://img.shields.io/badge/Plugman-Compatible-success.svg)](https://totalcmd.net/plugring/tc_plugman.html)
[![License](https://img.shields.io/badge/license-Free-lightgrey.svg)](#-license)

This plugin allows Total Commander to handle Arma Reforger `.PAK` archives as if they were regular folders. You can browse, search, and extract files with specialized support for game assets.

<img width="512" height="512" alt="ArmaPAK_Logo" src="https://github.com/user-attachments/assets/9f6c5e7a-5c4d-42ed-84d0-38aee0a3d048" />

---

### ✨ **Main Features**

- **Browse & View:** Open any `.pak` file and navigate its contents like a normal directory.
- **Quick Extraction Options (F5 / Alt+F9):** ⚡ A compact window pops up before copying or unpacking, allowing you to modify **extraction settings** on-the-fly. This prompt can be disabled in the settings for a silent workflow.
- **Smart Extract (Dependencies):** 🧠 When extracting models (`.xob`) or materials (`.emat`), the plugin automatically finds and extracts all required textures, provided they are located within the **currently active or opened archives**.
- **Intelligent Folders:** 📂 Optionally preserves the original internal folder hierarchy **during file copying and extraction** (`KeepDirectoryStructure`). It automatically detects if the target folder already exists to avoid creating redundant, duplicate folder levels.
- **Built-in Image Conversion:** Automatically converts game-specific `.edds` files to standard `.dds` format during extraction. 
  - *Tip:* You can view these images directly in Total Commander (**F3**) using [IrfanView](https://www.irfanview.com) with the [TC IrfanView Plugin](https://totalcmd.net/plugring/TCIrfanViewPlugin_2.0.html) installed.
- **Advanced Search:** Search for specific text inside archive files using Total Commander’s standard **Find Files** (`Alt + F7`) tool.

<img width="512" height="288" alt="ArmaPAK_Interface" src="https://github.com/user-attachments/assets/5ba5447a-5679-4c6f-94ff-4f50c6b98d38" />

---

### 🚀 **Installation**

#### **Automatic Method**
Simply open the **ArmaPAK-TC.zip** inside Total Commander and follow the automatic installation prompt.

#### **Manual Method**
1. Extract `ArmaPAK.wcx` (and `ArmaPAK.wcx64` for 64-bit systems) to your Total Commander `Plugins\wcx\` folder.
2. Launch Total Commander and go to: **Configuration** → **Options** → **Plugins**.
3. In the **Compressor Plugins (.WCX)** section, click **Configure**.
4. Enter **PAK** as the extension, click **New Type**, and select the `ArmaPAK.wcx` file.
5. Click **OK**.

---

### 📖 **How to Use**

- **Enter Archive:** Double-click or press **Enter** (or **Ctrl+PageDown**) on a `.pak` file.
- **Copying / Unpacking:** Use **F5** (Copy) or **Alt + F9** (Unpack). Use the pop-up window to adjust extraction parameters for the current task.
- **Searching:** Press **Alt + F7**, check "Find text", and the plugin will look inside the compressed files for you.
- **Configuration:** Fine-tune the plugin's behavior using the [Plugman](https://totalcmd.net/plugring/tc_plugman.html) utility or by editing the `pak_plugin.ini` file.

---

### 📄 **License**
This plugin is **free software**, provided **"as is."** You are free to redistribute it as long as the package remains unmodified.

---

### 🧑‍💻 **Author**
**Icebird** Copyright © 2026. All rights reserved.

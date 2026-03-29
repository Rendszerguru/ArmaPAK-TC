# 📦 Arma PAK Plugin for Total Commander

[![Version](https://img.shields.io/badge/version-1.1.5-blue.svg)](https://github.com/Rendszerguru/ArmaPAK-TC/releases/latest)
[![TC IrfanView](https://img.shields.io/badge/TC%20IrfanView-Plugin-orange.svg)](https://totalcmd.net/plugring/TCIrfanViewPlugin_2.0.html)
[![Plugman](https://img.shields.io/badge/Plugman-Compatible-success.svg)](https://totalcmd.net/plugring/tc_plugman.html)
[![License](https://img.shields.io/badge/license-Free-lightgrey.svg)](#-license)

Total Commander plugin that allows for managing Arma Reforger .PAK archives, reading files, **searching**, and extracting them.

<img width="512" height="512" alt="ArmaPAK_" src="https://github.com/user-attachments/assets/9f6c5e7a-5c4d-42ed-84d0-38aee0a3d048" />

### ✨ **Features**
- **Display the contents of PAK archives.**
- **Full-text search support inside PAK files:**
  Use Total Commander’s **Find Files** (`Alt + F7`) with **Find text** enabled to search within archive contents.
- **Automatic extraction of compressed files** (Zlib).
- **Quick Extract Options (F5):** ⚡
  A compact dialog appears before extraction, allowing you to toggle EDDS conversion and Smart Extract on-the-fly. This prompt can be disabled in the settings for a silent workflow.
- **Smart Extract (Dependency handling):** 🧠
  Automatically analyzes .xob and .emat files during extraction to identify and extract all required dependencies (like textures). It is capable of retrieving these files **across multiple active archives** within the same session. (Disabled by default)
- **Keep Directory Structure:** 📂
  Optionally preserves the original internal folder hierarchy during file copying and extraction, ensuring a clean and organized output.
- **EDDS to DDS conversion:** Automatically converts `.edds` image files to standard `.dds` format upon extraction.
  → While browsing the contents of a `.pak` file, the converted `.dds` images can be opened directly in **Total Commander’s viewer (F3)** using [IrfanView](https://www.irfanview.com) with the [TC IrfanView Plugin](https://totalcmd.net/plugring/TCIrfanViewPlugin_2.0.html) installed.
- **Advanced Configuration:** EDDS conversion, Smart Extraction, and Directory Structure settings can be managed in the `pak_plugin.ini` file or via the configuration dialog of [Plugman](https://totalcmd.net/plugring/tc_plugman.html).

<img width="512" height="264" alt="armapak_s" src="https://github.com/user-attachments/assets/7a60a739-cbfb-4fa1-a3e3-c7180bfd1474" />

- **Targeted error logging:** A `pak_plugin.log` file is created next to the plugin with automatic **log rotation** (5MB limit) to save disk space. Optional detailed logging can be enabled via `EnableLogInfo`.

---

### 🚀 **Installation**

#### **Automatic Installation**
Simply unzip the **ArmaPAK-TC.zip** file and press **Enter** to automatically install the plugin in Total Commander.

#### **Manual Installation**
If the automatic installation doesn’t work, follow these steps:

1. Extract the `ArmaPAK.wcx` (and `ArmaPAK.wcx64` for 64-bit) file into the Total Commander installation directory, under the **Plugins\wcx** subfolder.
   Example: `C:\TotalCommander\Plugins\wcx\ArmaPAK.wcx`

2. Launch Total Commander.
3. Navigate to: **Configuration menu** → **Options...** → **Plugins tab**.
4. In the right window, under the "Compressor Plugins (.WCX)" section, click the **Settings** button.
5. In the pop-up "Assign" window, enter **PAK** as a new extension.
6. Click **New Type**, then select the `ArmaPAK.wcx` file.
7. Click **OK**.

---

### 📖 **Usage**
- **Select a .pak file** and press **Enter** (or **Ctrl+PageDown**) to view its contents.
- **Extraction (F5):** When copying files, a quick options window appears (if enabled). You can confirm or change extraction settings (like EDDS conversion) for the current task.
- **Smart Extracting:** When you copy an `.xob` or `.emat` file, the plugin will automatically attempt to pull all related textures from the archives.
- **Search:** Press **Alt + F7**, enable **Find text**, and search directly within `.pak` contents.

---

### 📄 **License**
This plugin is **free software**, released **"as is."**
Use is at your own risk. Redistribution is permitted as long as the distribution remains intact.

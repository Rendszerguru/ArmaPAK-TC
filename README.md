# 📦 Arma PAK Plugin for Total Commander

[![Version](https://img.shields.io/badge/version-1.0-blue.svg)](https://github.com/Rendszerguru/ArmaPAK-TC/releases/latest)
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
- **EDDS to DDS conversion:** Automatically converts `.edds` image files to standard `.dds` format upon extraction.
  → While browsing the contents of a `.pak` file, the converted `.dds` images can be opened directly in **Total Commander’s viewer (F3)** using [IrfanView](https://www.irfanview.com) with the [TC IrfanView Plugin](https://totalcmd.net/plugring/TCIrfanViewPlugin_2.0.html) installed.
- **EDDS conversion settings:** EDDS conversion can be enabled or disabled in the `pak_plugin.ini` file alongside the plugin, and can also be toggled on or off via the configuration dialog of [Plugman](https://totalcmd.net/plugring/tc_plugman.html), the **Total Commander Plugins Manager**.

<img width="448" height="231" alt="ArmaPAKcfg30" src="https://github.com/user-attachments/assets/27c9efcf-b711-438f-93c5-f0da85d11631" />

- **Targeted error logging:** A `pak_plugin.log` file is created next to the plugin, recording only critical errors and session startup/shutdown information.
  Optional **detailed logging** can be enabled via `EnableLogInfo` for more verbose information.

---

### 🚀 **Installation**

#### **Automatic Installation**
Simply unzip the **ArmaPAK-TC.zip** file and press **Enter** to automatically install the plugin in Total Commander.

#### **Manual Installation**
If the automatic installation doesn’t work, follow these steps:

1. Extract the `ArmaPAK.wcx` file into the Total Commander installation directory, under the **Plugins\wcx** subfolder.
   Example: `C:\TotalCommander\Plugins\wcx\ArmaPAK.wcx`

2. Launch Total Commander.

3. Navigate to: **Configuration menu** → **Options...** → **Plugins tab**.

4. In the right window, under the "Compressor Plugins (.WCX)" section, click the **Settings** button (this is the general settings button in this section).

5. In the pop-up "Assign" window, enter **PAK** as a new extension.

6. Click **New Type**, then select the `ArmaPAK.wcx` file from the `Plugins\wcx\` folder.

7. Click **OK**.

---

### 📖 **Usage**
- **Select a .pak file** in Total Commander and press **Enter** to view its contents.
  - You can also open an archive with **Ctrl+PageDown**.
- To extract files, select the desired files or folders from the archive and use the **copy (F5)** command.
- To extract the entire archive, press **Alt + F9** on the `.pak` file.
- To **search inside an archive**, press **Alt + F7**, enable **Find text**, and search directly within `.pak` contents.

---

### 📄 **License**
This plugin is **free software**, released **"as is."**
Use is at your own risk. Redistribution is permitted as long as the distribution remains intact.

---

### 🧑‍💻 **Developer**
**Icebird**
Copyright © 2025 Icebird. All rights reserved.

If you encounter any issues with the plugin, please report them to the developer!

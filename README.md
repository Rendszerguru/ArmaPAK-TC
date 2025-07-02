# 📦 Arma PAK Plugin for Total Commander
Total Commander plugin that allows for managing Arma Reforger .PAK archives, viewing files, and extracting them, with automatic EDDS → DDS conversion.

![ArmaPAK_](https://github.com/user-attachments/assets/1355f9a1-9078-42a8-bb14-0abd2fa44812)

### ✨ **Features**
- **Display the contents of PAK archives.**
- **Automatic extraction of compressed files** (Zlib).
- **EDDS to DDS conversion**: Automatically converts .edds image files to standard .dds format upon extraction.
- **EDDS conversion settings**: EDDS conversion settings: EDDS conversion can be enabled or disabled in the pak_plugin.ini file alongside the plugin, and can also be toggled on or off via the configuration dialog of [Plugman](https://totalcmd.net/plugring/tc_plugman.html), the TC Plugins Manager.

![ArmaPAKcfg25](https://github.com/user-attachments/assets/9a097869-4ba0-4231-add9-ef39f7c3e483)
- **Targeted error logging**: A `pak_plugin.log` file is created next to the plugin, recording only critical errors and session startup/shutdown information, providing cleaner feedback on the operation.

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

- To extract files, select the desired files or folders from the archive and use the **copy (F5)** command. To extract the entire archive, press **Alt+F9** on the .pak file.

---

### 📄 **License**
This plugin is **free software**, released **"as is."** The use of the software is entirely at the user's own risk. The program may be freely copied and distributed, provided the integrity of the distribution is maintained.

---

### 🧑‍💻 **Developer**
**Icebird**

Copyright © 2025 Icebird. All rights reserved.

If you encounter any issues with the plugin, please report them to the developer!

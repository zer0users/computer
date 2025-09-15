#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fstream>

namespace fs = std::filesystem;

class ComputerVM {
private:
    std::string diskPath;
    std::string romPath;
    std::string firmwarePath;
    std::string noVNCPath;
    bool useVNC;
    pid_t qemuPid;
    pid_t websockifyPid;

public:
    ComputerVM() {
        diskPath = "./devices/disk/disk.qcow2";
        romPath = "./devices/rom";
        firmwarePath = "./boot/firmware/OVMF_CODE.fd";
        noVNCPath = "./libraries/noVNC";
        useVNC = true;
        qemuPid = -1;
        websockifyPid = -1;
    }

    void printLog(const std::string& level, const std::string& message) {
        std::cout << "[" << level << "] " << message << std::endl;
    }

    void printDebug(const std::string& message) {
        std::cout << "[DEBUG] " << message << std::endl;
    }

    bool checkFile(const std::string& path, const std::string& name) {
        if (fs::exists(path)) {
            printDebug(name + " available.. Yes!");
            return true;
        } else {
            printDebug(name + " available.. No");
            return false;
        }
    }

    std::string findISO() {
        printDebug("Checking for ISO files...");
        try {
            for (const auto& entry : fs::directory_iterator(romPath)) {
                if (entry.path().extension() == ".iso") {
                    return entry.path().string();
                }
            }
        } catch (const fs::filesystem_error& e) {
            printDebug("Error checking ROM directory: " + std::string(e.what()));
        }
        return "";
    }

    void createDirectories() {
        try {
            fs::create_directories("./devices/disk");
            fs::create_directories("./devices/rom");
            fs::create_directories("./boot/firmware");
            fs::create_directories("./libraries");
        } catch (const fs::filesystem_error& e) {
            printLog("ERROR", "Failed to create directories: " + std::string(e.what()));
        }
    }

    bool createDefaultDisk() {
        if (!fs::exists(diskPath)) {
            printLog("INFO", "Creating default 20GB disk...");
            std::string cmd = "qemu-img create -f qcow2 \"" + diskPath + "\" 20G";
            int result = system(cmd.c_str());
            if (result == 0) {
                printLog("INFO", "Default disk created successfully!");
                return true;
            } else {
                printLog("ERROR", "Failed to create default disk!");
                return false;
            }
        }
        return true;
    }

    std::vector<std::string> buildQEMUCommand() {
        std::vector<std::string> cmd;
        
        cmd.push_back("qemu-system-x86_64");
        
        // Básicos
        cmd.push_back("-enable-kvm");
        cmd.push_back("-cpu");
        cmd.push_back("host");
        cmd.push_back("-smp");
        cmd.push_back("4");
        cmd.push_back("-m");
        cmd.push_back("4G");
        
        // VirtIO para mejor rendimiento
        cmd.push_back("-vga");
        cmd.push_back("virtio");
        
        // Configurar display según modo
        if (useVNC) {
            cmd.push_back("-display");
            cmd.push_back("none");
            cmd.push_back("-vnc");
            cmd.push_back(":1");
        } else {
            cmd.push_back("-display");
            cmd.push_back("gtk,full-screen=on");
        }
        
        // UEFI Firmware con pflash - CORREGIDO
        if (fs::exists(firmwarePath)) {
            cmd.push_back("-drive");
            cmd.push_back("if=pflash,format=raw,readonly=on,file=" + firmwarePath);
            
            // Crear VARS file si no existe
            std::string varsPath = "./boot/firmware/OVMF_VARS.fd";
            if (!fs::exists(varsPath)) {
                printLog("INFO", "Creating OVMF VARS file...");
                std::ofstream varsFile(varsPath, std::ios::binary);
                std::vector<char> emptyVars(64 * 1024, 0);
                varsFile.write(emptyVars.data(), emptyVars.size());
                varsFile.close();
            }
            cmd.push_back("-drive");
            cmd.push_back("if=pflash,format=raw,file=" + varsPath);
        }
        
        // Disco principal
        if (fs::exists(diskPath)) {
            cmd.push_back("-drive");
            cmd.push_back("file=" + diskPath + ",format=qcow2,if=virtio");
        }
        
        // ISO si existe
        std::string isoFile = findISO();
        if (!isoFile.empty()) {
            cmd.push_back("-cdrom");
            cmd.push_back(isoFile);
            printLog("INFO", "ISO found: " + fs::path(isoFile).filename().string());
        }
        
        // Audio ALSA
        cmd.push_back("-audiodev");
        cmd.push_back("alsa,id=audio0");
        cmd.push_back("-device");
        cmd.push_back("intel-hda");
        cmd.push_back("-device");
        cmd.push_back("hda-duplex,audiodev=audio0");
        
        // Red con VirtIO
        cmd.push_back("-netdev");
        cmd.push_back("user,id=net0");
        cmd.push_back("-device");
        cmd.push_back("virtio-net-pci,netdev=net0");
        
        // USB para mejor mouse/teclado
        cmd.push_back("-device");
        cmd.push_back("usb-ehci");
        cmd.push_back("-device");
        cmd.push_back("usb-tablet");
        
        // Sincronización de tiempo
        cmd.push_back("-rtc");
        cmd.push_back("base=localtime,clock=host");
        
        return cmd;
    }

    bool startWebsockify() {
        if (!useVNC) return true;
        
        printLog("INFO", "Starting websockify for noVNC...");
        
        // Verificar que noVNC existe
        if (!fs::exists(noVNCPath)) {
            printLog("ERROR", "noVNC directory not found!");
            return false;
        }
        
        pid_t pid = fork();
        if (pid == 0) {
            // Proceso hijo - ejecutar websockify
            std::string cmd = "websockify --web=" + noVNCPath + " 8080 localhost:5901";
            execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
            exit(1);
        } else if (pid > 0) {
            websockifyPid = pid;
            sleep(2); // Dar tiempo para que inicie
            return true;
        } else {
            printLog("ERROR", "Failed to fork websockify process!");
            return false;
        }
    }

    bool startQEMU() {
        printLog("INFO", "Starting QEMU virtual machine...");
        
        auto cmd = buildQEMUCommand();
        
        // DEBUG: Imprimir comando completo
        printDebug("QEMU command:");
        std::string fullCmd = "";
        for (const auto& arg : cmd) {
            fullCmd += arg + " ";
        }
        printDebug(fullCmd);
        
        // Convertir a array de char* para execv
        std::vector<char*> args;
        for (const auto& arg : cmd) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        
        pid_t pid = fork();
        if (pid == 0) {
            // Proceso hijo - ejecutar QEMU
            execvp(args[0], args.data());
            exit(1);
        } else if (pid > 0) {
            qemuPid = pid;
            sleep(3); // Dar más tiempo a QEMU para iniciar VNC
            return true;
        } else {
            printLog("ERROR", "Failed to fork QEMU process!");
            return false;
        }
    }

    void cleanup() {
        if (qemuPid != -1) {
            kill(qemuPid, SIGTERM);
            waitpid(qemuPid, nullptr, 0);
        }
        if (websockifyPid != -1) {
            kill(websockifyPid, SIGTERM);
            waitpid(websockifyPid, nullptr, 0);
        }
    }

    bool boot() {
        printLog("LOG", "Booting Computer..");
        printDebug("Checking components..");
        
        // Crear directorios necesarios
        createDirectories();
        
        // Verificar componentes
        checkFile(firmwarePath, "Firmware");
        bool diskOk = checkFile(diskPath, "Disk") || createDefaultDisk();
        std::string isoFile = findISO();
        bool isoOk = !isoFile.empty();
        
        if (!isoOk) {
            printDebug("ISO available.. No");
        } else {
            printDebug("ISO available.. Yes");
        }
        
        if (!diskOk && !isoOk) {
            printLog("ERROR", "No disk or ISO available!");
            return false;
        }
        
        printDebug("Checking Libraries..");
        bool noVNCOk = checkFile(noVNCPath, "noVNC");
        bool websockifyOk = (system("which websockify > /dev/null 2>&1") == 0);
        printDebug(std::string("Websockify.. ") + (websockifyOk ? "Yes!" : "No"));
        
        if (useVNC && (!noVNCOk || !websockifyOk)) {
            printLog("ERROR", "Required libraries not found for VNC mode!");
            return false;
        }
        
        printDebug("Starting Machine..");
        
        if (isoOk && fs::exists(diskPath)) {
            printLog("INFO", "Booting from ISO with disk available!");
        } else if (isoOk) {
            printLog("INFO", "Booting from ISO only!");
        } else {
            printLog("INFO", "There's no ISO on rom/, Booting from disk!");
        }
        
        // Iniciar QEMU
        if (!startQEMU()) {
            return false;
        }
        
        // Iniciar websockify si usamos VNC
        if (useVNC) {
            if (!startWebsockify()) {
                cleanup();
                return false;
            }
            
            printLog("INFO", "Port 8080 For Machine Opened! Go to http://localhost:8080/vnc.html?resize=remote&autoconnect=true");
        } else {
            printLog("INFO", "Machine started in full-screen mode!");
        }
        
        return true;
    }

    void setVNCMode(bool enabled) {
        useVNC = enabled;
    }
};

void signalHandler(int /* sig */) {
    std::cout << "\n[INFO] Shutting down gracefully..." << std::endl;
    exit(0);
}

int main(int argc, char* argv[]) {
    // Configurar manejo de señales
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    ComputerVM vm;
    
    // Procesar argumentos
    bool noVNC = false;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--no-vnc") {
            noVNC = true;
            break;
        }
    }
    
    vm.setVNCMode(!noVNC);
    
    if (vm.boot()) {
        // Mantener el programa corriendo
        while (true) {
            sleep(1);
        }
    } else {
        std::cerr << "[ERROR] Failed to boot virtual machine!" << std::endl;
        return 1;
    }
    
    return 0;
}
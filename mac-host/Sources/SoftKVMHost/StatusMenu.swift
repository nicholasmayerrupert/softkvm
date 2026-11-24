import AppKit
import ApplicationServices
import ServiceManagement

/// The menu-bar icon and its menu. Rebuilt every time it opens so it always shows live state.
final class StatusMenu: NSObject, NSMenuDelegate {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let menu = NSMenu()
    private let server: Server

    init(server: Server) {
        self.server = server
        super.init()
        menu.delegate = self
        menu.autoenablesItems = false
        item.menu = menu
        refreshIcon()
        AppState.shared.onChange = { [weak self] in self?.refreshIcon() }
    }

    private func refreshIcon() {
        let connected = AppState.shared.clientName != nil
        let symbol = connected ? "display.2" : "display"
        if let image = NSImage(systemSymbolName: symbol, accessibilityDescription: "SoftKVM") {
            image.isTemplate = true
            item.button?.image = image
            item.button?.title = ""
        } else {
            item.button?.title = "KVM"
        }
        item.button?.appearsDisabled = !connected
        item.button?.toolTip = connected
            ? "SoftKVM — connected to \(AppState.shared.clientName ?? "PC")"
            : "SoftKVM — waiting for a PC"
    }

    // MARK: - Menu

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let state = AppState.shared

        menu.addItem(header("SoftKVM Host"))
        if let name = state.clientName {
            let addr = state.clientAddress.map { " (\($0))" } ?? ""
            menu.addItem(info("Connected to \(name)\(addr)"))
        } else {
            let addrs = AppState.localAddresses()
            menu.addItem(info(addrs.isEmpty ? "Waiting for a PC…" : "Waiting for a PC on \(addrs.joined(separator: ", "))…"))
        }

        if !state.monitors.isEmpty {
            menu.addItem(.separator())
            menu.addItem(header("Use these PC monitors for the Mac"))
            let selectedCount = state.monitors.filter(\.selected).count
            for m in state.monitors {
                let title = "\(m.name) — \(m.width)×\(m.height)"
                let mi = action(title, #selector(toggleMonitor(_:)))
                mi.tag = m.index
                mi.state = m.selected ? .on : .off
                // At least one monitor must stay selected.
                mi.isEnabled = !(m.selected && selectedCount == 1)
                mi.indentationLevel = 1
                menu.addItem(mi)
            }
        }

        menu.addItem(.separator())
        let quality = NSMenuItem(title: "Video quality", action: nil, keyEquivalent: "")
        let qualityMenu = NSMenu()
        for q in AppState.videoQualities {
            let mi = action("\(q.name) (\(q.mbps) Mbps)", #selector(setQuality(_:)))
            mi.tag = q.mbps
            mi.state = state.bitrateMbps == q.mbps ? .on : .off
            qualityMenu.addItem(mi)
        }
        quality.submenu = qualityMenu
        menu.addItem(quality)

        if state.clientName != nil {
            menu.addItem(action("Disconnect PC", #selector(disconnect)))
        }

        menu.addItem(.separator())
        let screenOK = CGPreflightScreenCaptureAccess()
        let axOK = AXIsProcessTrusted()
        let perms = NSMenuItem(title: screenOK && axOK ? "Permissions: all granted" : "Permissions: action needed ⚠︎",
                               action: nil, keyEquivalent: "")
        let permsMenu = NSMenu()
        let screen = action("Screen Recording \(screenOK ? "✓" : "— click to grant")", #selector(openScreenRecordingSettings))
        let ax = action("Accessibility \(axOK ? "✓" : "— click to grant")", #selector(openAccessibilitySettings))
        permsMenu.addItem(screen)
        permsMenu.addItem(ax)
        perms.submenu = permsMenu
        menu.addItem(perms)

        let login = action("Launch at Login", #selector(toggleLaunchAtLogin))
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
        menu.addItem(login)
        menu.addItem(action("Open Log", #selector(openLog)))

        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit SoftKVM Host", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        menu.addItem(quit)
    }

    private func header(_ title: String) -> NSMenuItem {
        let mi = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        mi.attributedTitle = NSAttributedString(string: title, attributes: [
            .font: NSFont.boldSystemFont(ofSize: NSFont.systemFontSize),
        ])
        mi.isEnabled = false
        return mi
    }

    private func info(_ title: String) -> NSMenuItem {
        let mi = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        mi.isEnabled = false
        return mi
    }

    private func action(_ title: String, _ selector: Selector) -> NSMenuItem {
        let mi = NSMenuItem(title: title, action: selector, keyEquivalent: "")
        mi.target = self
        return mi
    }

    // MARK: - Actions

    @objc private func toggleMonitor(_ sender: NSMenuItem) {
        var mask: UInt32 = 0
        for m in AppState.shared.monitors {
            let on = m.index == sender.tag ? !m.selected : m.selected
            if on { mask |= 1 << UInt32(m.index) }
        }
        guard mask != 0 else { return }
        server.requestMonitors(mask: mask)
    }

    @objc private func setQuality(_ sender: NSMenuItem) {
        AppState.shared.bitrateMbps = sender.tag
        server.setBitrate(sender.tag * 1_000_000)
    }

    @objc private func disconnect() { server.disconnect() }

    @objc private func openScreenRecordingSettings() {
        CGRequestScreenCaptureAccess()
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")!)
    }

    @objc private func openAccessibilitySettings() {
        _ = InputInjector.checkPermission(prompt: true)
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!)
    }

    @objc private func toggleLaunchAtLogin() {
        do {
            if SMAppService.mainApp.status == .enabled {
                try SMAppService.mainApp.unregister()
            } else {
                try SMAppService.mainApp.register()
            }
        } catch {
            Log.info("launch at login: \(error.localizedDescription)")
            let alert = NSAlert()
            alert.messageText = "Couldn't change Launch at Login"
            alert.informativeText = error.localizedDescription
            alert.runModal()
        }
    }

    @objc private func openLog() {
        let url = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/SoftKVMHost.log")
        NSWorkspace.shared.open(url)
    }
}

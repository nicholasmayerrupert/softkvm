import AppKit

/// Asks the user whether a new PC may control this Mac.
enum PairingPrompt {
    static func ask(name: String, code: String, reply: @escaping (Bool) -> Void) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Allow “\(name)” to control this Mac?"
        alert.informativeText = """
            It will be able to see this Mac's screen and use its keyboard and mouse.

            Only allow it if the PC is showing this code:

            \(code)
            """
        // "Don't Allow" is the default button, so an accidental Return can't approve a stranger.
        alert.addButton(withTitle: "Don't Allow")
        alert.addButton(withTitle: "Allow")
        reply(alert.runModal() == .alertSecondButtonReturn)
    }

    static func confirmForget(name: String) -> Bool {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "Forget “\(name)”?"
        alert.informativeText = "It will be disconnected and will need to be approved again to connect."
        alert.addButton(withTitle: "Forget")
        alert.addButton(withTitle: "Cancel")
        return alert.runModal() == .alertFirstButtonReturn
    }
}

import AppKit

/// Asks the user whether a new PC may control this Mac.
enum PairingPrompt {
    /// Codes whose PC went away while their prompt was up (any thread).
    private static let lock = NSLock()
    private static var cancelled = Set<String>()

    /// Closes the prompt for `code` if it's still up (the PC that asked went away).
    static func cancel(code: String) {
        lock.lock(); cancelled.insert(code); lock.unlock()
    }

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
        // The modal loop only runs timers scheduled for modal-panel mode; poll for cancellation there.
        let watcher = Timer(timeInterval: 0.25, repeats: true) { _ in
            lock.lock(); let gone = cancelled.remove(code) != nil; lock.unlock()
            if gone { NSApp.abortModal() }
        }
        RunLoop.main.add(watcher, forMode: .modalPanel)
        let response = alert.runModal()
        watcher.invalidate()
        lock.lock(); cancelled.remove(code); lock.unlock()
        reply(response == .alertSecondButtonReturn)
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

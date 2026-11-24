import Foundation

/// Minimal logger: stdout plus ~/Library/Logs/SoftKVMHost.log.
enum Log {
    private static let queue = DispatchQueue(label: "softkvm.log")
    private static let handle: FileHandle? = {
        let url = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs/SoftKVMHost.log")
        FileManager.default.createFile(atPath: url.path, contents: nil)
        return try? FileHandle(forWritingTo: url)
    }()
    private static let formatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    static func info(_ message: @autoclosure () -> String) {
        let line = "\(formatter.string(from: Date())) \(message())\n"
        queue.async {
            FileHandle.standardOutput.write(line.data(using: .utf8)!)
            handle?.write(line.data(using: .utf8)!)
        }
    }
}

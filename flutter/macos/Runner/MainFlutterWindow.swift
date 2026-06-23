import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    self.contentViewController = flutterViewController

    let screenFrame = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 800, height: 600)
    let width: CGFloat = 390
    let height: CGFloat = 844
    let x = screenFrame.midX - width / 2
    let y = screenFrame.midY - height / 2
    self.setFrame(NSRect(x: x, y: y, width: width, height: height), display: true)
    self.contentMinSize = NSSize(width: 300, height: 600)

    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}

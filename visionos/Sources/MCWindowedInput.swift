import Foundation

// Small value-only mapper so held input, disconnect releases and frame-time
// limits can be exercised without a controller, a guest or a headset.
struct MCWindowedPadSample {
    var leftX: Double = 0, leftY: Double = 0
    var rightX: Double = 0, rightY: Double = 0
    var jump = false, inventory = false, pause = false
    var attack = false, use = false, previousSlot = false, nextSlot = false
}

enum MCWindowedPadEvent: Equatable {
    case key(Int32, Bool), button(Int32, Bool)
    case move(Double, Double), scroll(Double)
}

struct MCWindowedPadState {
    private(set) var keys = Set<Int32>()
    private(set) var buttons = Set<Int32>()
    private var previousSlot = false, nextSlot = false

    static func limitedDelta(_ dt: Double) -> Double {
        dt.isFinite ? max(0, min(0.05, dt)) : 0
    }
    private func axis(_ value: Double, key: Int32) -> Bool {
        value.isFinite && value > (keys.contains(key) ? 0.18 : 0.28)
    }
    private func look(_ value: Double) -> Double {
        guard value.isFinite else { return 0 }
        let v = min(1, abs(value))
        guard v > 0.18 else { return 0 }
        return (value < 0 ? -1 : 1) * pow(min(1, (v - 0.18) / 0.82), 1.5)
    }
    mutating func step(_ s: MCWindowedPadSample, dt: Double, grabbed: Bool) -> [MCWindowedPadEvent] {
        var result: [MCWindowedPadEvent] = []
        let wants: [(Int32, Bool)] = [
            (87, axis(s.leftY, key: 87)), (83, axis(-s.leftY, key: 83)),
            (65, axis(-s.leftX, key: 65)), (68, axis(s.leftX, key: 68)),
            (32, s.jump), (69, s.inventory), (256, s.pause)
        ]
        for (key, down) in wants where down != keys.contains(key) {
            if down { keys.insert(key) } else { keys.remove(key) }
            result.append(.key(key, down))
        }
        for (button, down): (Int32, Bool) in [(0, s.attack), (1, s.use)] where down != buttons.contains(button) {
            if down { buttons.insert(button) } else { buttons.remove(button) }
            result.append(.button(button, down))
        }
        let slot = (s.previousSlot && !previousSlot ? 1.0 : 0) - (s.nextSlot && !nextSlot ? 1.0 : 0)
        previousSlot = s.previousSlot; nextSlot = s.nextSlot
        if slot != 0 { result.append(.scroll(slot)) }
        let speed = grabbed ? 900.0 : 700.0
        let dx = look(s.rightX) * speed * Self.limitedDelta(dt)
        let dy = -look(s.rightY) * speed * Self.limitedDelta(dt)
        if dx != 0 || dy != 0 { result.append(.move(dx, dy)) }
        return result
    }
    mutating func release() -> [MCWindowedPadEvent] {
        let out = keys.sorted().map { MCWindowedPadEvent.key($0, false) }
            + buttons.sorted().map { MCWindowedPadEvent.button($0, false) }
        keys.removeAll(); buttons.removeAll(); previousSlot = false; nextSlot = false
        return out
    }
}

#if os(visionOS)
import GameController
import SwiftUI

// One input owner for all windows, with no immersive compositor or ARKit
// session. Inactive/removed windows, disconnects and game exit all release
// held state before the next owner can send anything.
@MainActor
final class MCWindowedInput: ObservableObject {
    static let shared = MCWindowedInput()
    @Published private(set) var controllerName: String?
    private var windows: [UUID: Bool] = [:]
    private var timer: Timer?
    private var observers: [NSObjectProtocol] = []
    private weak var controller: GCController?
    private var state = MCWindowedPadState()
    private var previousTime: TimeInterval?
    private var sending = false

    var hint: String {
        controllerName == nil
            ? "Pair a DualSense to play in this window. Pinch selects menu items."
            : "Left stick move · Right stick look · Cross jump · Triangle inventory · R2 select/attack · L2 use · L1/R1 hotbar · Circle/Options pause"
    }

    private init() {
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] notification in
                let disconnected = notification.name == .GCControllerDidDisconnect
                    ? (notification.object as? GCController).map(ObjectIdentifier.init) : nil
                Task { @MainActor in self?.chooseController(excluding: disconnected) }
            })
        }
    }

    func window(_ id: UUID, active: Bool) {
        windows[id] = active
        if !active { release(force: true) }
        updatePump()
    }
    func removeWindow(_ id: UUID) {
        windows.removeValue(forKey: id)
        release(force: true)
        updatePump()
    }
    private func updatePump() {
        guard windows.values.contains(true) else {
            timer?.invalidate(); timer = nil
            release(force: true); return
        }
        guard timer == nil else { return }
        chooseController()
        previousTime = nil
        let t = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        timer = t
        RunLoop.main.add(t, forMode: .common)
    }
    private func chooseController(excluding disconnected: ObjectIdentifier? = nil) {
        guard !windows.isEmpty else { return }
        let candidates = GCController.controllers().filter {
            if ObjectIdentifier($0) == disconnected { return false }
            guard $0.extendedGamepad != nil else { return false }
            if #available(visionOS 26.0, *), $0.productCategory == GCProductCategorySpatialController { return false }
            let name = ($0.vendorName ?? "").lowercased()
            return !name.contains("psvr2") && !name.contains("ps vr2")
        }
        if let controller, candidates.contains(where: { $0 === controller }) { return }
        release()
        controller = candidates.first
        controllerName = controller?.vendorName ?? (controller == nil ? nil : "Game controller")
    }
    private func tick() {
        guard windows.values.contains(true), kl_app_native_windowed_ready() != 0,
              let gamepad = controller?.extendedGamepad else {
            if sending { release() }
            return
        }
        sending = true
        let now = ProcessInfo.processInfo.systemUptime
        let dt = previousTime.map { now - $0 } ?? (1.0 / 60.0)
        previousTime = now
        // Resume after a delayed main thread with releases, rather than a
        // backlog of look or held inputs from before focus was lost.
        if dt > 0.25 { release(); previousTime = now; return }
        var sample = MCWindowedPadSample()
        sample.leftX = Double(gamepad.leftThumbstick.xAxis.value)
        sample.leftY = Double(gamepad.leftThumbstick.yAxis.value)
        sample.rightX = Double(gamepad.rightThumbstick.xAxis.value)
        sample.rightY = Double(gamepad.rightThumbstick.yAxis.value)
        sample.jump = gamepad.buttonA.isPressed
        sample.inventory = gamepad.buttonY.isPressed
        sample.pause = gamepad.buttonB.isPressed || gamepad.buttonMenu.isPressed
        sample.attack = gamepad.rightTrigger.isPressed
        sample.use = gamepad.leftTrigger.isPressed
        sample.previousSlot = gamepad.leftShoulder.isPressed
        sample.nextSlot = gamepad.rightShoulder.isPressed
        for event in state.step(sample, dt: dt, grabbed: kl_app_native_windowed_cursor_mode() == 0x34003) {
            send(event)
        }
    }
    private func send(_ event: MCWindowedPadEvent) {
        switch event {
        case let .key(key, down): kl_app_native_windowed_key(key, down ? 1 : 0)
        case let .button(button, down): kl_app_native_windowed_button(button, down ? 1 : 0)
        case let .move(x, y): kl_app_native_windowed_move(x, y)
        case let .scroll(y): kl_app_native_windowed_scroll(y)
        }
    }
    private func release(force: Bool = false) {
        for event in state.release() { send(event) }
        if sending || force { kl_app_native_windowed_release_all() }
        sending = false
        previousTime = nil
    }
}
#endif

import CoreWLAN
import Foundation

guard let wifi = CWWiFiClient.shared().interface(), wifi.powerOn() else {
    print("Wi-Fi radio unavailable. Check that Wi-Fi is turned on.")
    exit(1)
}

func securityDescription(_ network: CWNetwork) -> String {
    if [.enterprise, .wpaEnterprise, .wpaEnterpriseMixed, .wpa2Enterprise, .wpa3Enterprise, .dynamicWEP].contains(where: network.supportsSecurity) {
        return "enterprise authentication (ask the organizer for connection details)"
    }
    if [.personal, .wpaPersonal, .wpaPersonalMixed, .wpa2Personal, .wpa3Personal, .wpa3Transition, .WEP].contains(where: network.supportsSecurity) {
        return "Wi-Fi password required"
    }
    if [.none, .OWE, .oweTransition].contains(where: network.supportsSecurity) {
        return "no Wi-Fi password required"
    }
    return "security unknown"
}

let args = Array(CommandLine.arguments.dropFirst())
let checkSecurity = args.first == "--security"
if checkSecurity && args.count != 2 {
    print("Usage: swift scan-stick-wifi.swift --security 'network name'")
    exit(2)
}
for name in checkSecurity ? [args[1]] : ["OAI-Events", "Hermes iPhone"] {
    do {
        let matches = try wifi.scanForNetworks(withSSID: Data(name.utf8))
        if matches.isEmpty {
            print("\(name): no access points returned")
        }
        for network in matches {
            if checkSecurity {
                let channel = network.wlanChannel.map { String($0.channelNumber) } ?? "unknown"
                print("\(name), channel \(channel): \(securityDescription(network))")
                continue
            }
            guard let channel = network.wlanChannel else {
                print("\(name): detected, band unknown")
                continue
            }
            let band: String
            switch channel.channelBand {
            case .band2GHz: band = "2.4 GHz"
            case .band5GHz: band = "5 GHz"
            default: band = "other or unknown band"
            }
            print("\(name): \(band), channel \(channel.channelNumber), signal \(network.rssiValue) dBm")
        }
    } catch {
        print("\(name): scan failed — \(error.localizedDescription)")
    }
}

if CommandLine.arguments.contains("--nearby") {
    do {
        let networks = try wifi.scanForNetworks(withSSID: nil)
        let nearby = networks.filter { $0.wlanChannel?.channelBand == .band2GHz }
        print("Nearby 2.4 GHz access points: \(nearby.count)")
        for network in nearby.sorted(by: { $0.rssiValue > $1.rssiValue }) {
            let name = network.ssid?.debugDescription ?? "(name unavailable)"
            print("  \(name), channel \(network.wlanChannel!.channelNumber), signal \(network.rssiValue) dBm, \(securityDescription(network))")
        }
    } catch {
        print("Nearby scan failed: \(error.localizedDescription)")
    }
}

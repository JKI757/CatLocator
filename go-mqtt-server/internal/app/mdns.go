package app

import (
	"fmt"
	"os"
	"strings"

	"github.com/grandcat/zeroconf"
)

const (
	mdnsServiceType = "_catlocator._tcp"
	mdnsDomain      = "local."
)

func (a *App) startMDNS(port int) error {
	if port <= 0 {
		return fmt.Errorf("invalid port %d", port)
	}

	a.stopMDNS()

	hostname, err := os.Hostname()
	if err != nil || hostname == "" {
		hostname = "catlocator"
	}

	instance := sanitizeMDNSInstance(fmt.Sprintf("CatLocator Server (%s)", hostname))
	hostLabel := sanitizeMDNSHost(hostname)
	hostFQDN := hostLabel
	if !strings.Contains(hostFQDN, ".") {
		hostFQDN = hostLabel + ".local"
	}

	txt := []string{
		fmt.Sprintf("mqtt_port=%d", port),
		fmt.Sprintf("http_port=%d", a.cfg.HTTPPort),
		"tls=0",
		"proto=v1",
		fmt.Sprintf("host=%s", hostFQDN),
	}

	server, err := zeroconf.Register(instance, mdnsServiceType, mdnsDomain, port, txt, nil)
	if err != nil {
		return err
	}

	a.mdns = server
	a.logger.Info("mDNS advertisement started", "instance", instance, "port", port)
	return nil
}

func (a *App) stopMDNS() {
	if a.mdns == nil {
		return
	}

	a.mdns.Shutdown()
	a.logger.Info("mDNS advertisement stopped")
	a.mdns = nil
}

func sanitizeMDNSInstance(name string) string {
	cleaned := strings.TrimSpace(name)
	cleaned = strings.ReplaceAll(cleaned, "\n", " ")
	cleaned = strings.ReplaceAll(cleaned, "\r", " ")
	cleaned = strings.ReplaceAll(cleaned, ".", " ")
	cleaned = strings.ReplaceAll(cleaned, "_", " ")
	if cleaned == "" {
		cleaned = "CatLocator Server"
	}
	runes := []rune(cleaned)
	const maxLen = 63
	if len(runes) > maxLen {
		cleaned = string(runes[:maxLen])
	}
	return cleaned
}

func sanitizeMDNSHost(name string) string {
	cleaned := strings.TrimSpace(strings.ToLower(name))
	replacer := strings.NewReplacer(" ", "-", "_", "-", "\n", "", "\r", "")
	cleaned = replacer.Replace(cleaned)
	if cleaned == "" {
		cleaned = "catlocator"
	}
	// Host labels must be <=63 characters.
	irunes := []rune(cleaned)
	if len(irunes) > 63 {
		cleaned = string(irunes[:63])
	}
	return cleaned
}

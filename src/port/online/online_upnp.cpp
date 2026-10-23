// PORT-ONLY (checkpoint 132on2e): see online_upnp.hpp.
#include "online_upnp.hpp"

#include "online_tcp.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

namespace omega::online {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ip_text(std::uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xff) + "." + std::to_string((ip >> 8) & 0xff) + "." +
           std::to_string(ip & 0xff);
}

// The text between <tag> and </tag> (namespace prefixes allowed: <u:tag>), searching from `from`.
std::optional<std::string> xml_value(const std::string& xml, const std::string& tag, std::size_t from = 0,
                                     std::size_t* end_out = nullptr) {
    const std::string lx = lower(xml);
    const std::string lt = lower(tag);
    std::size_t pos = from;
    while ((pos = lx.find(lt, pos)) != std::string::npos) {
        const std::size_t open = lx.rfind('<', pos);
        const bool named = open != std::string::npos && pos + lt.size() < lx.size() &&
                           (lx[pos + lt.size()] == '>' || lx[pos + lt.size()] == ' ') && lx[open + 1] != '/' &&
                           (open + 1 == pos || lx[pos - 1] == ':');
        if (!named) {
            pos += lt.size();
            continue;
        }
        const std::size_t start = lx.find('>', pos);
        if (start == std::string::npos) return std::nullopt;
        std::size_t close = lx.find("</", start);
        while (close != std::string::npos) {
            const std::size_t gt = lx.find('>', close);
            if (gt == std::string::npos) return std::nullopt;
            std::string name = lx.substr(close + 2, gt - close - 2);
            if (const auto colon = name.find(':'); colon != std::string::npos) name = name.substr(colon + 1);
            if (name == lt) {
                if (end_out) *end_out = gt + 1;
                std::string v = xml.substr(start + 1, close - start - 1);
                const auto b = v.find_first_not_of(" \t\r\n");
                const auto e = v.find_last_not_of(" \t\r\n");
                return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
            }
            close = lx.find("</", gt);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

int http_status(const std::string& response) {
    const auto sp = response.find(' ');
    return sp == std::string::npos ? 0 : std::atoi(response.c_str() + sp + 1);
}

std::optional<std::string> http(const UpnpOptions& o, std::uint32_t ip, std::uint16_t port, const std::string& req) {
    if (o.http) return o.http(ip, port, req, o.http_timeout_ms);
    return tcp_exchange(ip, port, req, o.http_timeout_ms);
}

std::string soap_request(const UpnpUrl& url, const std::string& service, const std::string& action,
                         const std::string& args) {
    const std::string body = "<?xml version=\"1.0\"?>\r\n"
                             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                             action + " xmlns:u=\"" + service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    return "POST " + url.path + " HTTP/1.1\r\nHost: " + ip_text(url.ip) + ":" + std::to_string(url.port) +
           "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" + action +
           "\"\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

struct SoapResult {
    bool ok = false;
    std::string body;
    std::string error;  // "no answer" / "HTTP 500 UPnPError 718 ConflictInMappingEntry"
    int upnp_code = 0;
};

SoapResult soap(const UpnpOptions& o, const UpnpMapping& m, const std::string& action, const std::string& args) {
    SoapResult r;
    const auto resp = http(o, m.control_ip, m.control_port, soap_request({m.control_ip, m.control_port, m.control_path},
                                                                          m.service_type, action, args));
    if (!resp) {
        r.error = "no answer from " + ip_text(m.control_ip) + ":" + std::to_string(m.control_port);
        return r;
    }
    r.body = upnp_http_body(*resp);
    const int status = http_status(*resp);
    if (status == 200) {
        r.ok = true;
        return r;
    }
    r.upnp_code = std::atoi(xml_value(r.body, "errorCode").value_or("0").c_str());
    r.error = "HTTP " + std::to_string(status);
    if (r.upnp_code) r.error += " UPnPError " + std::to_string(r.upnp_code) + " " + xml_value(r.body, "errorDescription").value_or("");
    return r;
}

}  // namespace

std::optional<std::string> upnp_header(const std::string& response, const std::string& name) {
    const std::string lr = lower(response);
    const std::string key = "\n" + lower(name) + ":";
    const auto pos = lr.find(key);
    if (pos == std::string::npos) return std::nullopt;
    const auto start = pos + key.size();
    const auto end = response.find_first_of("\r\n", start);
    std::string v = response.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const auto b = v.find_first_not_of(" \t");
    return b == std::string::npos ? std::string() : v.substr(b);
}

std::optional<UpnpUrl> upnp_parse_url(const std::string& url) {
    if (lower(url.substr(0, 7)) != "http://") return std::nullopt;
    const std::string rest = url.substr(7);
    const auto slash = rest.find('/');
    const std::string hostport = rest.substr(0, slash);
    UpnpUrl u;
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto hp = split_host_port(hostport, 80);
    if (!hp) return std::nullopt;
    const auto ip = resolve_ipv4(hp->first);
    if (!ip) return std::nullopt;
    u.ip = *ip;
    u.port = hp->second;
    return u;
}

std::optional<std::pair<std::string, std::string>> upnp_find_wan_service(const std::string& xml) {
    std::size_t pos = 0;
    std::optional<std::pair<std::string, std::string>> ppp;
    for (;;) {
        std::size_t end = 0;
        const auto type = xml_value(xml, "serviceType", pos, &end);
        if (!type) break;
        pos = end;
        const bool ip = type->find("service:WANIPConnection:") != std::string::npos;
        const bool pp = type->find("service:WANPPPConnection:") != std::string::npos;
        if (!ip && !pp) continue;
        const auto control = xml_value(xml, "controlURL", pos);
        if (!control) continue;
        if (ip) return std::make_pair(*type, *control);
        if (!ppp) ppp = std::make_pair(*type, *control);
    }
    return ppp;
}

std::string upnp_http_body(const std::string& response) {
    const auto split = response.find("\r\n\r\n");
    if (split == std::string::npos) return {};
    std::string body = response.substr(split + 4);
    const auto te = upnp_header(response.substr(0, split + 2), "Transfer-Encoding");
    if (!te || lower(*te).find("chunked") == std::string::npos) return body;
    std::string out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        const auto eol = body.find("\r\n", pos);
        if (eol == std::string::npos) break;
        const std::size_t n = std::strtoul(body.substr(pos, eol - pos).c_str(), nullptr, 16);
        if (n == 0) break;
        out += body.substr(eol + 2, n);
        pos = eol + 2 + n + 2;
    }
    return out;
}

UpnpMapping upnp_map_udp(std::uint16_t internal_port, const UpnpOptions& o,
                         const std::function<void(const std::string&)>& log) {
    auto say = [&](const std::string& line) {
        if (log) log(line);
    };
    UpnpMapping m;
    m.internal_port = internal_port;
    // 1. SSDP discovery.
    UdpSocket ssdp;
    std::string error;
    if (!ssdp.open(0, 0, &error)) {
        m.error = "SSDP socket: " + error;
        return m;
    }
    for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                           "urn:schemas-upnp-org:device:InternetGatewayDevice:2"}) {
        const std::string search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " +
                                   std::string(st) + "\r\n\r\n";
        ssdp.send_to(o.ssdp_ip, o.ssdp_port, std::span<const std::uint8_t>(
                                                  reinterpret_cast<const std::uint8_t*>(search.data()), search.size()));
    }
    std::optional<UpnpUrl> location;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.discover_ms);
    while (!location && std::chrono::steady_clock::now() < deadline) {
        auto d = ssdp.receive();
        if (!d) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const std::string text(d->bytes.begin(), d->bytes.end());
        const auto st = upnp_header(text, "ST");
        const auto loc = upnp_header(text, "LOCATION");
        if (!loc || (st && lower(*st).find("internetgatewaydevice") == std::string::npos)) continue;
        location = upnp_parse_url(*loc);
        if (location) m.gateway = *loc;
    }
    ssdp.close();
    if (!location) {
        m.error = "no UPnP router answered the SSDP search in " + std::to_string(o.discover_ms) + " ms";
        return m;
    }
    say("UPnP: router at " + m.gateway);
    // 2. The device description: the WAN connection service's control URL.
    const auto desc = http(o, location->ip, location->port,
                           "GET " + location->path + " HTTP/1.1\r\nHost: " + ip_text(location->ip) + ":" +
                               std::to_string(location->port) + "\r\nConnection: close\r\n\r\n");
    if (!desc || http_status(*desc) != 200) {
        m.error = "the router's description " + m.gateway + " did not load";
        return m;
    }
    const std::string xml = upnp_http_body(*desc);
    const auto service = upnp_find_wan_service(xml);
    if (!service) {
        m.error = "the router lists no WANIPConnection / WANPPPConnection service";
        return m;
    }
    m.service_type = service->first;
    std::string control = service->second;
    if (lower(control.substr(0, 7)) != "http://") {
        std::string base = xml_value(xml, "URLBase").value_or("");
        if (base.empty()) base = "http://" + ip_text(location->ip) + ":" + std::to_string(location->port);
        while (!base.empty() && base.back() == '/') base.pop_back();
        control = base + (control.empty() || control[0] != '/' ? "/" : "") + control;
    }
    const auto curl = upnp_parse_url(control);
    if (!curl) {
        m.error = "bad control URL " + control;
        return m;
    }
    m.control_ip = curl->ip;
    m.control_port = curl->port;
    m.control_path = curl->path;
    const auto local = local_ip_toward(curl->ip);
    if (!local) {
        m.error = "no local address toward the router";
        return m;
    }
    m.internal_ip = *local;
    // 3. AddPortMapping: the same external port, else the next few (718: ConflictInMappingEntry).
    std::string last_error;
    for (int i = 0; i < std::max(1, o.port_tries) && !m.mapped; ++i) {
        const auto ext = static_cast<std::uint16_t>(internal_port + i);
        if (ext == 0) break;
        const SoapResult r =
            soap(o, m, "AddPortMapping",
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(ext) +
                     "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(internal_port) +
                     "</NewInternalPort><NewInternalClient>" + ip_text(*local) +
                     "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" + o.description +
                     "</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>");
        if (r.ok) {
            m.mapped = true;
            m.external_port = ext;
        } else {
            last_error = r.error;
            if (r.upnp_code != 718) break;
        }
    }
    if (!m.mapped) {
        m.error = "AddPortMapping refused: " + last_error;
        return m;
    }
    // 4. The router's external address (for the log; omega_server uses the address it sees).
    const SoapResult x = soap(o, m, "GetExternalIPAddress", "");
    if (x.ok) m.external_ip = xml_value(x.body, "NewExternalIPAddress").value_or("");
    return m;
}

bool upnp_unmap(const UpnpMapping& m, const UpnpOptions& o, std::string* error) {
    if (!m.mapped) return true;
    const SoapResult r = soap(o, m, "DeletePortMapping",
                              "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(m.external_port) +
                                  "</NewExternalPort><NewProtocol>UDP</NewProtocol>");
    if (!r.ok && error) *error = r.error;
    return r.ok;
}

}  // namespace omega::online

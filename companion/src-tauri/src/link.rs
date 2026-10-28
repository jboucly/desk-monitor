//! WebSocket link to the screen: hello / pairing / metrics / actions (see protocol/README.md, "PC link").

use crate::actions;
use crate::config::{Config, ConfigStore};
use crate::metrics::Collector;
use futures_util::{SinkExt, StreamExt};
use serde::Serialize;
use serde_json::{json, Value};
use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tauri::{AppHandle, Emitter};
use tokio::sync::mpsc;
use tokio_tungstenite::tungstenite::Message;

/// What the window shows. Sent to it as the "link-state" event.
#[derive(Clone, Debug, Serialize)]
pub struct LinkState {
    /// connecting, connected, pair_required, pairing, offline
    pub status: String,
    /// Address being used
    pub host: String,
    /// Address typed by the user (empty: automatic)
    pub custom_host: String,
    pub paired: bool,
    /// Last error or hint, in French
    pub message: String,
}

/// Requests from the window to the link task.
#[derive(Debug)]
pub enum Command {
    StartPairing,
    SubmitCode(String),
    SetHost(String),
    Forget,
}

/// Shared handle used by the Tauri commands.
#[derive(Clone)]
pub struct Link {
    pub state: Arc<Mutex<LinkState>>,
    pub commands: mpsc::UnboundedSender<Command>,
}

const RETRY_DELAY: Duration = Duration::from_secs(3);
const METRICS_PERIOD: Duration = Duration::from_secs(1);

/// Starts the link task. It reconnects forever.
pub fn start(app: AppHandle, store: ConfigStore) -> Link {
    let config = store.load();
    let state = Arc::new(Mutex::new(LinkState {
        status: "connecting".into(),
        host: config.host_or_default(),
        custom_host: config.host.clone(),
        paired: !config.token.is_empty(),
        message: String::new(),
    }));
    let (tx, rx) = mpsc::unbounded_channel();
    let link = Link { state: state.clone(), commands: tx };
    tauri::async_runtime::spawn(run(app, store, config, state, rx));
    link
}

/// Updates the shared state and tells the window.
fn publish(app: &AppHandle, state: &Arc<Mutex<LinkState>>, update: impl FnOnce(&mut LinkState)) {
    let snapshot = {
        let mut s = state.lock().unwrap();
        update(&mut s);
        s.clone()
    };
    let _ = app.emit("link-state", snapshot);
}

/// Why a connection ended.
enum End {
    Retry,
    Reconnect,
}

async fn run(
    app: AppHandle,
    store: ConfigStore,
    mut config: Config,
    state: Arc<Mutex<LinkState>>,
    mut commands: mpsc::UnboundedReceiver<Command>,
) {
    let collector = Arc::new(Mutex::new(Collector::new()));
    loop {
        let host = config.host_or_default();
        publish(&app, &state, |s| {
            s.status = "connecting".into();
            s.host = host.clone();
            s.custom_host = config.host.clone();
            s.paired = !config.token.is_empty();
        });
        let end = match connect(&host, &config.last_address).await {
            Ok((socket, address)) => {
                log::info!("connected to {address}");
                if config.last_address != address.to_string() {
                    config.last_address = address.to_string();
                    store.save(&config);
                }
                session(&app, &store, &mut config, &state, &mut commands, &collector, socket).await
            }
            Err(e) => {
                log::warn!("cannot reach {host}: {e}");
                publish(&app, &state, |s| {
                    s.status = "offline".into();
                    s.message = format!("Écran introuvable à l'adresse {host} ({e})");
                });
                End::Retry
            }
        };
        if let End::Retry = end {
            // Wait, but react at once to a new address typed meanwhile
            tokio::select! {
                _ = tokio::time::sleep(RETRY_DELAY) => {}
                Some(cmd) = commands.recv() => apply_offline_command(&store, &mut config, cmd),
            }
        }
    }
}

/// Commands received while not connected: only the address and "forget" make sense.
fn apply_offline_command(store: &ConfigStore, config: &mut Config, cmd: Command) {
    match cmd {
        Command::SetHost(host) => {
            config.host = host;
            config.last_address.clear();
            store.save(config);
        }
        Command::Forget => {
            config.token.clear();
            store.save(config);
        }
        Command::StartPairing | Command::SubmitCode(_) => {}
    }
}

type Socket = tokio_tungstenite::WebSocketStream<tokio::net::TcpStream>;

/// Splits "host" or "host:port" (port 80 by default).
fn split_host(host: &str) -> (String, u16) {
    match host.rsplit_once(':') {
        Some((name, port)) if !name.contains(':') => match port.parse() {
            Ok(port) => (name.to_string(), port),
            Err(_) => (host.to_string(), 80),
        },
        _ => (host.trim_matches(['[', ']']).to_string(), 80),
    }
}

/// IPv4 addresses of a name. Asking for IPv4 only matters for "desk-monitor.local": a lookup of any
/// family also waits for the IPv6 answer, which the screen never sends (seconds of delay).
fn lookup_ipv4(name: &str, port: u16) -> Vec<SocketAddr> {
    let hints = dns_lookup::AddrInfoHints {
        flags: 0,
        address: dns_lookup::AddrFamily::Inet.into(),
        socktype: dns_lookup::SockType::Stream.into(),
        protocol: 0,
    };
    match dns_lookup::getaddrinfo(Some(name), Some(&port.to_string()), Some(hints)) {
        Ok(found) => found.filter_map(Result::ok).map(|info| info.sockaddr).collect(),
        Err(_) => Vec::new(),
    }
}

/// Addresses to try, in order.
async fn resolve(name: &str, port: u16) -> Result<Vec<SocketAddr>, String> {
    if let Ok(ip) = name.parse::<std::net::IpAddr>() {
        return Ok(vec![SocketAddr::new(ip, port)]);
    }
    let owned = name.to_string();
    let ipv4 = tokio::task::spawn_blocking(move || lookup_ipv4(&owned, port));
    if let Ok(Ok(found)) = tokio::time::timeout(Duration::from_secs(6), ipv4).await {
        if !found.is_empty() {
            return Ok(found);
        }
    }
    // No IPv4 answer: any family, as a last resort
    match tokio::time::timeout(Duration::from_secs(8), tokio::net::lookup_host((name, port))).await {
        Ok(Ok(found)) => Ok(found.collect()),
        Ok(Err(e)) => Err(format!("nom introuvable ({e})")),
        Err(_) => Err("nom introuvable (délai dépassé)".into()),
    }
}

/// Connects to the screen: the last address that worked first, then the resolved ones.
/// Returns the socket and the address that answered.
async fn connect(host: &str, last_address: &str) -> Result<(Socket, SocketAddr), String> {
    let (name, port) = split_host(host);
    let mut last_error = String::from("aucune adresse");

    let cached: Option<SocketAddr> = last_address.parse().ok();
    if let Some(address) = cached {
        match attempt(address, host).await {
            Ok(socket) => return Ok((socket, address)),
            Err(e) => last_error = e,
        }
    }
    let mut addresses = match resolve(&name, port).await {
        Ok(found) => found,
        Err(e) if cached.is_some() => return Err(format!("{last_error} ; {e}")),
        Err(e) => return Err(e),
    };
    addresses.sort_by_key(|a| a.is_ipv6());
    addresses.dedup();
    for address in addresses.into_iter().filter(|a| Some(*a) != cached) {
        match attempt(address, host).await {
            Ok(socket) => return Ok((socket, address)),
            Err(e) => last_error = e,
        }
    }
    Err(last_error)
}

/// Opens the TCP connection and the WebSocket on one address.
async fn attempt(address: SocketAddr, host: &str) -> Result<Socket, String> {
    let stream = match tokio::time::timeout(Duration::from_secs(3), tokio::net::TcpStream::connect(address)).await {
        Ok(Ok(stream)) => stream,
        Ok(Err(e)) => return Err(format!("{address} : {e}")),
        Err(_) => return Err(format!("{address} : délai dépassé")),
    };
    let _ = stream.set_nodelay(true);
    let url = format!("ws://{host}/api/pc");
    match tokio::time::timeout(Duration::from_secs(5), tokio_tungstenite::client_async(url.as_str(), stream)).await {
        Ok(Ok((socket, _))) => Ok(socket),
        Ok(Err(e)) => Err(format!("{address} : {e}")),
        Err(_) => Err(format!("{address} : pas de réponse WebSocket")),
    }
}

/// Name shown on the screen: the computer name on macOS, the host name elsewhere.
fn computer_name() -> String {
    #[cfg(target_os = "macos")]
    if let Ok(out) = std::process::Command::new("/usr/sbin/scutil").args(["--get", "ComputerName"]).output() {
        let name = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if !name.is_empty() {
            return name;
        }
    }
    hostname::get()
        .map(|h| h.to_string_lossy().trim_end_matches(".local").to_string())
        .unwrap_or_else(|_| "PC".into())
}

fn os_name() -> &'static str {
    if cfg!(target_os = "macos") {
        "macos"
    } else if cfg!(windows) {
        "windows"
    } else {
        "linux"
    }
}

async fn session(
    app: &AppHandle,
    store: &ConfigStore,
    config: &mut Config,
    state: &Arc<Mutex<LinkState>>,
    commands: &mut mpsc::UnboundedReceiver<Command>,
    collector: &Arc<Mutex<Collector>>,
    socket: Socket,
) -> End {
    let (mut write, mut read) = socket.split();
    // Action results come back from other tasks through this channel
    let (out_tx, mut out_rx) = mpsc::unbounded_channel::<Value>();

    let hello = json!({
        "type": "hello",
        "name": computer_name(),
        "os": os_name(),
        "version": env!("CARGO_PKG_VERSION"),
        "token": config.token,
    });
    if write.send(Message::text(hello.to_string())).await.is_err() {
        return End::Retry;
    }

    let mut welcomed = false;
    let mut ticker = tokio::time::interval(METRICS_PERIOD);
    loop {
        let outgoing: Option<Value> = tokio::select! {
            incoming = read.next() => {
                let text = match incoming {
                    Some(Ok(Message::Text(text))) => text.to_string(),
                    Some(Ok(Message::Close(_))) | None | Some(Err(_)) => break,
                    Some(Ok(_)) => continue,
                };
                let Ok(msg) = serde_json::from_str::<Value>(&text) else { continue };
                handle_message(app, store, config, state, &out_tx, &mut welcomed, &msg)
            }
            _ = ticker.tick(), if welcomed => {
                let collector = collector.clone();
                tokio::task::spawn_blocking(move || collector.lock().unwrap().sample()).await.ok()
            }
            Some(result) = out_rx.recv() => Some(result),
            Some(cmd) = commands.recv() => match cmd {
                Command::StartPairing => Some(json!({ "type": "pair_start" })),
                Command::SubmitCode(code) => Some(json!({ "type": "pair", "code": code.trim() })),
                Command::SetHost(host) => {
                    config.host = host;
                    config.last_address.clear();
                    store.save(config);
                    return End::Reconnect;
                }
                Command::Forget => {
                    config.token.clear();
                    store.save(config);
                    return End::Reconnect;
                }
            },
        };
        if let Some(message) = outgoing {
            if write.send(Message::text(message.to_string())).await.is_err() {
                break;
            }
        }
    }
    publish(app, state, |s| {
        s.status = "offline".into();
        s.message = "Connexion à l'écran perdue".into();
    });
    End::Retry
}

/// Reacts to one message of the screen. Returns a message to send back, if any.
fn handle_message(
    app: &AppHandle,
    store: &ConfigStore,
    config: &mut Config,
    state: &Arc<Mutex<LinkState>>,
    out: &mpsc::UnboundedSender<Value>,
    welcomed: &mut bool,
    msg: &Value,
) -> Option<Value> {
    match msg.get("type").and_then(Value::as_str).unwrap_or("") {
        "welcome" => {
            *welcomed = true;
            publish(app, state, |s| {
                s.status = "connected".into();
                s.paired = true;
                s.message.clear();
            });
            None
        }
        "pair_required" => {
            publish(app, state, |s| {
                s.status = "pair_required".into();
                s.paired = false;
                s.message = if config.token.is_empty() {
                    String::new()
                } else {
                    "L'écran ne reconnaît plus ce PC : appaire-le à nouveau".into()
                };
            });
            None
        }
        "pair_pending" => {
            publish(app, state, |s| {
                s.status = "pairing".into();
                s.message.clear();
            });
            None
        }
        "paired" => {
            config.token = msg.get("token").and_then(Value::as_str).unwrap_or("").to_string();
            store.save(config);
            None
        }
        "error" => {
            let code = msg.get("code").and_then(Value::as_str).unwrap_or("");
            let text = msg.get("message").and_then(Value::as_str).unwrap_or("Erreur").to_string();
            publish(app, state, |s| {
                if code == "pair_expired" {
                    s.status = "pair_required".into();
                }
                s.message = text;
            });
            None
        }
        "action" => {
            let id = msg.get("id").cloned().unwrap_or(Value::Null);
            let action = msg.get("action").cloned().unwrap_or(Value::Null);
            run_action(app.clone(), id, action, out.clone());
            None
        }
        _ => None,
    }
}

/// Runs an action off the link task and queues its result. Key presses run on the main thread.
fn run_action(app: AppHandle, id: Value, action: Value, out: mpsc::UnboundedSender<Value>) {
    let reply = move |result: Result<(), String>| {
        if let Err(e) = &result {
            log::warn!("action failed: {e}");
        }
        let _ = out.send(match result {
            Ok(()) => json!({ "type": "action_result", "id": id, "ok": true }),
            Err(e) => json!({ "type": "action_result", "id": id, "ok": false, "error": e }),
        });
    };
    if actions::is_keys(&action) {
        if !crate::accessibility_granted(false) {
            reply(Err("Autorise l'app Desk Monitor dans Réglages Système, Accessibilité".into()));
            return;
        }
        let keys = action.get("keys").and_then(Value::as_str).unwrap_or("").to_string();
        let _ = app.run_on_main_thread(move || reply(actions::press_keys(&keys)));
    } else {
        tauri::async_runtime::spawn_blocking(move || reply(actions::run(&action)));
    }
}

#[cfg(test)]
mod tests {
    use super::split_host;

    #[test]
    fn splits_host_and_port() {
        assert_eq!(split_host("desk-monitor.local"), ("desk-monitor.local".into(), 80));
        assert_eq!(split_host("localhost:8081"), ("localhost".into(), 8081));
        assert_eq!(split_host("192.168.1.50"), ("192.168.1.50".into(), 80));
    }
}

//! Local HTTP server for the Claude Code hooks (`http` hooks installed by `install.rs`).
//!
//! - `POST /hook`: state events (SessionStart, PreToolUse, Stop…), answered at once.
//! - `POST /permission`: a PermissionRequest. The request is held until the screen answers, the
//!   approval is settled in the terminal, or about 100 s pass. An empty answer lets Claude Code ask
//!   in the terminal as usual.
//! - `POST /statusline`: the status line input, for the plan limits (`rate_limits`).
//!
//! Only 127.0.0.1 is listened on, and every request must carry the `X-DM-Token` secret, so that a web
//! page cannot post approvals.

use super::{Ask, Decision, Shared};
use axum::extract::State;
use axum::http::{HeaderMap, StatusCode};
use axum::routing::post;
use axum::{Json, Router};
use serde_json::{json, Value};
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::oneshot;

/// How long an approval waits for the screen. Claude Code's hook timeout (set at install) is longer.
const ASK_TIMEOUT: Duration = Duration::from_secs(100);

/// Serves the hooks forever. Logs and gives up when the port is taken.
pub async fn serve(shared: Arc<Shared>, port: u16) {
    let app = Router::new()
        .route("/hook", post(hook))
        .route("/permission", post(permission))
        .route("/statusline", post(statusline))
        .with_state(shared);
    match tokio::net::TcpListener::bind(("127.0.0.1", port)).await {
        Ok(listener) => {
            log::info!("Claude Code hooks on http://127.0.0.1:{port}");
            if let Err(e) = axum::serve(listener, app).await {
                log::error!("Claude Code hook server stopped: {e}");
            }
        }
        Err(e) => log::error!("cannot listen on 127.0.0.1:{port} for the Claude Code hooks: {e}"),
    }
}

/// Removes an approval from the screen when its request ends, answered or cancelled.
struct AskGuard {
    shared: Arc<Shared>,
    id: String,
}

impl Drop for AskGuard {
    fn drop(&mut self) {
        self.shared.remove_ask(&self.id);
    }
}

fn authorized(shared: &Shared, headers: &HeaderMap) -> bool {
    headers.get("x-dm-token").and_then(|v| v.to_str().ok()).is_some_and(|t| !t.is_empty() && t == shared.token)
}

async fn hook(State(shared): State<Arc<Shared>>, headers: HeaderMap, Json(event): Json<Value>) -> Result<Json<Value>, StatusCode> {
    if !authorized(&shared, &headers) {
        return Err(StatusCode::UNAUTHORIZED);
    }
    shared.apply_hook(&event);
    Ok(Json(json!({})))
}

async fn statusline(State(shared): State<Arc<Shared>>, headers: HeaderMap, Json(input): Json<Value>) -> Result<Json<Value>, StatusCode> {
    if !authorized(&shared, &headers) {
        return Err(StatusCode::UNAUTHORIZED);
    }
    if let Some(limits) = super::statusline_limits(&input["rate_limits"]) {
        *shared.statusline_limits.lock().unwrap() = Some(limits);
        shared.wake();
    }
    Ok(Json(json!({})))
}

async fn permission(State(shared): State<Arc<Shared>>, headers: HeaderMap, Json(event): Json<Value>) -> Result<Json<Value>, StatusCode> {
    if !authorized(&shared, &headers) {
        return Err(StatusCode::UNAUTHORIZED);
    }
    shared.apply_hook(&event);
    // No screen to ask: the terminal asks at once
    if !shared.screen_connected() {
        return Ok(Json(json!({})));
    }
    let (reply, answer) = oneshot::channel();
    let id = shared.add_ask(Ask::from_event(&event, reply));
    // Claude Code drops the request when the approval is answered in its own dialog (a refusal sends
    // no other hook): the handler is then cancelled at the await, and the guard still removes the ask
    let _guard = AskGuard { shared: shared.clone(), id };
    let decision = tokio::time::timeout(ASK_TIMEOUT, answer).await.ok().and_then(Result::ok);
    Ok(Json(match decision {
        Some(Decision::Allow) => json!({
            "hookSpecificOutput": { "hookEventName": "PermissionRequest", "decision": { "behavior": "allow" } }
        }),
        Some(Decision::Deny) => json!({
            "hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": { "behavior": "deny", "message": "Refusé depuis le Desk Monitor" }
            }
        }),
        Some(Decision::Ask) | None => json!({}),
    }))
}

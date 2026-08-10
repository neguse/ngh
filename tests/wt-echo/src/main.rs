//! WebTransport echo server for the ngh interop tests.
//!
//! Usage: wt-echo [port]   (default 4433)
//!
//! Serves a self-signed certificate and prints `cert_sha256=<hex>` on
//! startup -- the client pins that hash, serverCertificateHashes-style.
//! Echoes every datagram and every bidirectional stream of any session.

use wtransport::endpoint::IncomingSession;
use wtransport::{Endpoint, Identity, ServerConfig};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let port: u16 = std::env::args()
        .nth(1)
        .map(|s| s.parse())
        .transpose()?
        .unwrap_or(4433);
    let identity = Identity::self_signed(["localhost", "127.0.0.1", "::1"])?;
    let hash = identity.certificate_chain().as_slice()[0].hash();
    let hex: String = hash.as_ref().iter().map(|b| format!("{b:02x}")).collect();
    println!("cert_sha256={hex}");
    let config = ServerConfig::builder()
        .with_bind_default(port)
        .with_identity(identity)
        .build();
    let server = Endpoint::server(config)?;
    println!("wt-echo listening on udp/{port}");
    for id in 0u64.. {
        let incoming = server.accept().await;
        tokio::spawn(handle(incoming, id));
    }
    Ok(())
}

async fn handle(incoming: IncomingSession, id: u64) {
    match handle_inner(incoming, id).await {
        Ok(()) => println!("[{id}] closed"),
        Err(e) => println!("[{id}] error: {e}"),
    }
}

async fn handle_inner(incoming: IncomingSession, id: u64) -> anyhow::Result<()> {
    let request = incoming.await?;
    println!(
        "[{id}] session request: authority={} path={}",
        request.authority(),
        request.path()
    );
    let conn = request.accept().await?;
    println!("[{id}] session established");
    loop {
        tokio::select! {
            d = conn.receive_datagram() => {
                let d = d?;
                conn.send_datagram(d.payload())?;
            }
            s = conn.accept_bi() => {
                let (mut tx, mut rx) = s?;
                tokio::spawn(async move {
                    let mut buf = vec![0u8; 65536];
                    loop {
                        match rx.read(&mut buf).await {
                            Ok(Some(n)) => {
                                if tx.write_all(&buf[..n]).await.is_err() {
                                    break;
                                }
                            }
                            Ok(None) => {
                                let _ = tx.finish().await;
                                break;
                            }
                            Err(_) => break,
                        }
                    }
                });
            }
        }
    }
}

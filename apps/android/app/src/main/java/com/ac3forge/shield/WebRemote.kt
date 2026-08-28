package com.ac3forge.shield

import android.util.Log
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStream
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.net.URLDecoder

private const val TAG = "ShieldAtmosDemo"

/**
 * A phone in the room, as a second controller.
 *
 * The demo's biggest practical limitation with an audience is that only one
 * person can hold the pad, and explaining a D-pad axis-mode toggle to someone
 * who just walked in costs more than the demo is worth. A page anyone can open
 * with a touchpad on it removes the explaining, the pairing and the handing
 * over. It is also the natural landing site for the roadmap's own "real live
 * object-position source" item, which currently names this app as the only
 * controller-driven path.
 *
 * **Off by default, and started only on an explicit keypress.** A demo app has
 * no business opening a listening socket on somebody's network because it
 * happened to launch. When it is running the URL is on screen, and there is
 * nothing to discover otherwise.
 *
 * Deliberately tiny and deliberately narrow. This is not a web server: it
 * answers exactly five fixed paths, ignores everything else with a 404, reads
 * only a bounded request line, serves one page held as a constant, and never
 * touches the filesystem. There is no authentication - anyone who can reach
 * the port can move the object - which is the honest trade for a sideloaded
 * personal demo on a home network, and is why it does not start by itself.
 */
class WebRemote(private val onCommand: (Command) -> Unit) {

    sealed interface Command {
        data class Move(val dx: Float, val dy: Float, val dz: Float) : Command
        data class Scene(val delta: Int) : Command
        data object ToggleObjects : Command
        data object Snap : Command
    }

    @Volatile
    private var server: ServerSocket? = null

    @Volatile
    private var thread: Thread? = null

    val isRunning: Boolean get() = server != null

    /** The URL to put on screen, or null if not running / no usable address. */
    @Volatile
    var url: String? = null
        private set

    fun start(): Boolean {
        if (isRunning) return true
        return try {
            val socket = ServerSocket(PORT)
            socket.reuseAddress = true
            server = socket
            url = localAddress()?.let { "http://$it:$PORT" }
            thread = Thread({ serve(socket) }, "web-remote").apply {
                isDaemon = true
                start()
            }
            Log.i(TAG, "web remote listening on ${url ?: "port $PORT"}")
            true
        } catch (t: Throwable) {
            // Port in use, no permission, no network - all the same answer
            // here: the demo carries on without it.
            Log.w(TAG, "web remote failed to start", t)
            server = null
            url = null
            false
        }
    }

    fun stop() {
        val socket = server ?: return
        server = null
        url = null
        // Closing the socket is what unblocks accept(); the thread then sees a
        // null `server` and exits rather than treating it as an error.
        try {
            socket.close()
        } catch (t: Throwable) {
            Log.w(TAG, "web remote close failed", t)
        }
        thread = null
        Log.i(TAG, "web remote stopped")
    }

    private fun serve(socket: ServerSocket) {
        while (server === socket) {
            val client = try {
                socket.accept()
            } catch (e: SocketException) {
                break  // stop() closed it; not an error
            } catch (t: Throwable) {
                Log.w(TAG, "web remote accept failed", t)
                break
            }
            try {
                client.soTimeout = CLIENT_TIMEOUT_MS
                handle(client)
            } catch (t: Throwable) {
                Log.w(TAG, "web remote request failed", t)
            } finally {
                try {
                    client.close()
                } catch (_: Throwable) {
                }
            }
        }
    }

    private fun handle(client: Socket) {
        val reader = BufferedReader(InputStreamReader(client.getInputStream()))
        val requestLine = reader.readLine() ?: return
        if (requestLine.length > MAX_REQUEST_LINE) {
            respond(client.getOutputStream(), "414 URI Too Long", "text/plain", "too long")
            return
        }
        // "GET /path?query HTTP/1.1" - only the target is of interest, and
        // headers and body are ignored entirely: every command this accepts
        // fits in a query string.
        val target = requestLine.split(' ').getOrNull(1) ?: return
        val path = target.substringBefore('?')
        val query = target.substringAfter('?', "")
        val out = client.getOutputStream()

        when (path) {
            "/" -> respond(out, "200 OK", "text/html; charset=utf-8", PAGE)
            "/move" -> {
                onCommand(
                    Command.Move(
                        query.floatParam("dx"),
                        query.floatParam("dy"),
                        query.floatParam("dz"),
                    ),
                )
                respond(out, "200 OK", "text/plain", "ok")
            }
            "/scene" -> {
                onCommand(Command.Scene(if (query.floatParam("d") < 0f) -1 else 1))
                respond(out, "200 OK", "text/plain", "ok")
            }
            "/objects" -> {
                onCommand(Command.ToggleObjects)
                respond(out, "200 OK", "text/plain", "ok")
            }
            "/snap" -> {
                onCommand(Command.Snap)
                respond(out, "200 OK", "text/plain", "ok")
            }
            else -> respond(out, "404 Not Found", "text/plain", "no")
        }
    }

    /**
     * One query parameter as a float, clamped to [-1, 1] and defaulting to 0.
     *
     * Clamped here rather than trusted: this is the only place data from off
     * the device reaches the demo, and the native side takes these as movement
     * deltas. Anything unparseable is 0, which is the harmless answer.
     */
    private fun String.floatParam(name: String): Float {
        val raw = split('&')
            .firstOrNull { it.startsWith("$name=") }
            ?.substringAfter('=')
            ?: return 0f
        val decoded = try {
            URLDecoder.decode(raw, "UTF-8")
        } catch (t: Throwable) {
            return 0f
        }
        val value = decoded.toFloatOrNull() ?: return 0f
        if (value.isNaN() || value.isInfinite()) return 0f
        return value.coerceIn(-1f, 1f)
    }

    private fun respond(out: OutputStream, status: String, contentType: String, body: String) {
        val bytes = body.toByteArray(Charsets.UTF_8)
        val header = buildString {
            append("HTTP/1.1 ").append(status).append("\r\n")
            append("Content-Type: ").append(contentType).append("\r\n")
            append("Content-Length: ").append(bytes.size).append("\r\n")
            append("Cache-Control: no-store\r\n")
            append("Connection: close\r\n\r\n")
        }
        out.write(header.toByteArray(Charsets.US_ASCII))
        out.write(bytes)
        out.flush()
    }

    private fun localAddress(): String? = try {
        NetworkInterface.getNetworkInterfaces()
            .asSequence()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.inetAddresses.asSequence() }
            .filterIsInstance<Inet4Address>()
            .firstOrNull { !it.isLoopbackAddress }
            ?.hostAddress
    } catch (t: Throwable) {
        Log.w(TAG, "could not determine local address", t)
        null
    }

    companion object {
        private const val PORT = 8642
        private const val CLIENT_TIMEOUT_MS = 4000
        private const val MAX_REQUEST_LINE = 2048

        // One page, no external anything - the Shield is not on the internet
        // for this and the phone loading it may not be either. Touch events
        // are throttled to roughly one request per animation frame, matching
        // what InputController already does with the physical stick: the
        // native side wants at most one deflection per frame, not one per
        // touch sample.
        private val PAGE = """
<!doctype html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Shield Atmos Demo</title>
<style>
 :root { color-scheme: dark; }
 body { margin:0; background:#08090b; color:#eef1f5;
        font:16px/1.4 system-ui,-apple-system,sans-serif;
        height:100dvh; display:flex; flex-direction:column; gap:12px; padding:14px;
        box-sizing:border-box; overscroll-behavior:none; }
 h1 { font-size:15px; letter-spacing:.14em; text-transform:uppercase;
      color:#5ebeff; margin:0; text-align:center; }
 #pad { flex:1; border-radius:22px; background:#161a1c;
        border:2px solid #282c32; position:relative; touch-action:none; }
 #dot { position:absolute; width:34px; height:34px; margin:-17px 0 0 -17px;
        border-radius:50%; background:#ff635b; left:50%; top:50%;
        box-shadow:0 0 26px rgba(255,99,91,.55); }
 #hint { text-align:center; color:#969ea8; font-size:13px; }
 .row { display:flex; gap:10px; }
 button { flex:1; padding:16px 8px; font-size:15px; border-radius:14px;
          border:1px solid #282c32; background:#161a1c; color:#eef1f5; }
 button:active { background:#26303a; }
 #h { width:100%; }
</style></head><body>
<h1>Shield Atmos Demo</h1>
<div id="pad"><div id="dot"></div></div>
<div id="hint">Drag on the pad to push the object &mdash; let go and it drifts back</div>
<input id="h" type="range" min="-1" max="1" step="0.01" value="0">
<div class="row">
  <button onclick="go('/scene?d=-1')">&larr; Scene</button>
  <button onclick="go('/snap')">Reset</button>
  <button onclick="go('/scene?d=1')">Scene &rarr;</button>
</div>
<div class="row"><button onclick="go('/objects')">Objects on / off</button></div>
<script>
 const pad = document.getElementById('pad'), dot = document.getElementById('dot');
 const h = document.getElementById('h');
 let dx = 0, dy = 0, active = false;
 function go(u) { fetch(u).catch(function(){}); }
 function at(e) {
   const r = pad.getBoundingClientRect();
   const t = e.touches ? e.touches[0] : e;
   dx = Math.max(-1, Math.min(1, ((t.clientX - r.left) / r.width) * 2 - 1));
   dy = Math.max(-1, Math.min(1, ((t.clientY - r.top) / r.height) * 2 - 1));
   dot.style.left = ((dx + 1) / 2 * 100) + '%';
   dot.style.top = ((dy + 1) / 2 * 100) + '%';
 }
 function release() { active = false; dx = 0; dy = 0; dot.style.left = '50%'; dot.style.top = '50%'; }
 pad.addEventListener('pointerdown', function(e){ active = true; at(e); pad.setPointerCapture(e.pointerId); });
 pad.addEventListener('pointermove', function(e){ if (active) at(e); });
 pad.addEventListener('pointerup', release);
 pad.addEventListener('pointercancel', release);
 // One request per animation frame while anything is non-zero, never one per
 // touch sample - the native side coalesces to one deflection per frame anyway.
 function tick() {
   const dz = parseFloat(h.value) || 0;
   if (active || dz !== 0) {
     go('/move?dx=' + dx.toFixed(3) + '&dy=' + dy.toFixed(3) + '&dz=' + dz.toFixed(3));
   }
   requestAnimationFrame(tick);
 }
 requestAnimationFrame(tick);
</script></body></html>
""".trimIndent()
    }
}

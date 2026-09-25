# Install a Hearth sink from the browser

This page puts `hearth_sink` onto an ESP32 board over its USB cable, from the newest release,
with nothing to install on the computer. It uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
and the browser's Web Serial, so it needs Chrome, Edge or Firefox on a computer. Safari cannot
do it, and neither can anything on iOS.

[The sink firmware guide](sink-firmware.md) covers the same steps with `esptool` and without a
browser, updating a board over its network afterwards, and what to do when a board does not come
back.

<!-- Raw HTML (md_in_html), so its paths are relative to this page's built URL,
     hearth/sink-installer/ with directory URLs on: "../../assets/" is the site's
     assets. docs.yml fills assets/sink-installer/ when it deploys, with ESP Web
     Tools and the newest release's firmware (tools/hearth/installer_site.py),
     so a local `mkdocs build` shows the page with nothing to install. -->
<script type="module" src="../../assets/sink-installer/esp-web-tools/install-button.js"></script>

<div id="sink-installer" markdown="0">
  <p id="sink-installer-status"><em>Looking for the newest release's firmware…</em></p>
  <ul id="sink-installer-images"></ul>
</div>

<script>
(async () => {
  const base = "../../assets/sink-installer/";
  const status = document.getElementById("sink-installer-status");
  const list = document.getElementById("sink-installer-images");
  let index;
  try {
    const response = await fetch(base + "index.json", { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`index.json answered ${response.status}`);
    }
    index = await response.json();
  } catch (error) {
    status.textContent = "This copy of the site carries no installer firmware (" + error.message +
      "). The published site has it once a release publishes sink firmware.";
    return;
  }
  if (!Array.isArray(index.images) || index.images.length === 0) {
    status.textContent = "No release publishes sink firmware yet. Until one does, build the " +
      "firmware as the board's guide describes.";
    return;
  }
  status.textContent = index.version
    ? "Firmware from release " + index.version + ". Choose your board:"
    : "Firmware from the newest release. Choose your board:";
  for (const image of index.images) {
    const item = document.createElement("li");
    const button = document.createElement("esp-web-install-button");
    button.setAttribute("manifest", base + image.manifest);
    const activate = document.createElement("button");
    activate.setAttribute("slot", "activate");
    activate.className = "md-button md-button--primary";
    activate.textContent = "Install on an " + image.title;
    button.appendChild(activate);
    const unsupported = document.createElement("span");
    unsupported.setAttribute("slot", "unsupported");
    unsupported.textContent = "This browser has no Web Serial: use Chrome, Edge or Firefox on a computer.";
    button.appendChild(unsupported);
    const notAllowed = document.createElement("span");
    notAllowed.setAttribute("slot", "not-allowed");
    notAllowed.textContent = "Web Serial works only on a page served over HTTPS.";
    button.appendChild(notAllowed);
    item.appendChild(button);
    list.appendChild(item);
  }
})();
</script>

## Before you start

- **Which board you have.** The four images, and how to tell boards apart, are in
  [Which image](sink-firmware.md#which-image). The installer checks the chip before it writes
  anything: an image for another chip is refused.
- **A USB cable to the board's serial connector.** On an ESP32-S3 DevKitC-1 that is the
  connector marked USB. On a FireBeetle 2 ESP32-P4 it is the USB-C connector that carries the
  console; the board's other connector is a separate USB peripheral.

## Installing

1. Connect the board, press the button for it above, and choose its port.
2. **Erase, or keep the board's settings.** The installer asks.
   - A new board: erase it.
   - A board that already runs `hearth_sink`: don't erase it. It keeps its name, its network and
     its pairings, and moves to the two-slot flash layout that updates over the network need.
3. **If the installer cannot connect,** put the chip into its download mode by hand: hold BOOT,
   press and release RESET, release BOOT, and try again. This mode is in the chip's ROM, which
   nothing can overwrite, so it works even on a board whose firmware no longer starts.
4. When the writing is done, press RESET once if the board was put into download mode by hand.
5. The installer then offers to give the board its Wi-Fi network over
   [Improv](https://www.improv-wifi.com/), which the firmware answers. The board stores the
   network and joins it.

From then on the board's page is at `http://hearth-XXXXXX.local/`, where `XXXXXX` is the end of
its MAC address. Newer releases go onto it over the network: see
[Updating over the network](sink-firmware.md#updating-over-the-network).

## Third-party notices

The installer is [ESP Web Tools](https://github.com/esphome/esp-web-tools) 10.4.0, by the ESPHome
project, under the Apache License 2.0. The site serves its own copy, with the licence beside it
(`assets/sink-installer/esp-web-tools/LICENSE`), rather than loading it from a CDN.

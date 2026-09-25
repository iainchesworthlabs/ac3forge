# Install a Hearth sink from the browser

This page puts `hearth_sink` onto an ESP32-S3, ESP32-C6 or ESP32-P4 board over its USB cable,
from the newest release, with nothing to install on the computer. It uses
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) and the browser's Web Serial, so it
needs Chrome, Edge or Firefox on a computer. Safari cannot do it, and neither can anything on iOS.

The site copies the images from the release when it is published, and each release publishes it
again. A browser cannot fetch a release's files from another site, so they are served from here.
The page also asks GitHub whether a newer release has firmware, and says so if one does.

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
  <p id="sink-installer-newer" hidden></p>
  <div id="sink-installer-boards"></div>
</div>

<script>
(async () => {
  const base = "../../assets/sink-installer/";
  // Every chip the firmware is published for, in the order the page shows
  // them. Each is always listed, so a chip with no image says so rather than
  // going missing.
  const chips = ["ESP32-S3", "ESP32-C6", "ESP32-P4"];
  const status = document.getElementById("sink-installer-status");
  const newer = document.getElementById("sink-installer-newer");
  const boards = document.getElementById("sink-installer-boards");
  const link = (href, text) => {
    const a = document.createElement("a");
    a.href = href;
    a.textContent = text;
    return a;
  };
  const day = (stamp) => (stamp || "").slice(0, 10);

  let index = null;
  let problem = "";
  try {
    const response = await fetch(base + "index.json", { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`index.json answered ${response.status}`);
    }
    index = await response.json();
  } catch (error) {
    problem = error.message;
  }
  const images = index && Array.isArray(index.images) ? index.images : [];
  const release = index ? index.tag || index.version || "" : "";

  status.textContent = "";
  if (index === null) {
    status.textContent = "This copy of the site carries no installer firmware (" + problem +
      "). The published site takes it from the newest release that has sink firmware.";
  } else if (images.length === 0) {
    status.textContent = "No release publishes sink firmware yet. Until one does, build the " +
      "firmware as the board's guide describes.";
  } else {
    status.append("Firmware from ");
    status.append(index.page ? link(index.page, "release " + release) : release || "the newest release");
    if (index.published) {
      status.append(", published " + day(index.published));
    }
    status.append(". Choose your board:");
  }

  for (const chip of chips) {
    const section = document.createElement("section");
    const heading = document.createElement("h3");
    heading.textContent = chip;
    section.appendChild(heading);
    const mine = images.filter((image) => image.chip === chip);
    if (mine.length === 0) {
      const none = document.createElement("p");
      none.textContent = images.length === 0
        ? "No image yet."
        : (release ? "Release " + release : "This release") + " has no image for the " + chip + ".";
      section.appendChild(none);
    }
    const list = document.createElement("ul");
    for (const image of mine) {
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
    if (mine.length > 0) {
      section.appendChild(list);
    }
    boards.appendChild(section);
  }

  // Whether a release newer than the one the site took has firmware: GitHub's
  // API answers other sites, which its release files do not. The page works
  // without this, so a failure here says nothing.
  if (index && index.repository && index.manifest) {
    try {
      const response = await fetch(
        `https://api.github.com/repos/${index.repository}/releases?per_page=30`,
        { headers: { Accept: "application/vnd.github+json" } });
      if (response.ok) {
        const newest = (await response.json()).find((entry) =>
          (entry.assets || []).some((asset) => asset.name === index.manifest));
        if (newest && newest.tag_name !== index.tag) {
          newer.textContent = "";
          newer.append(link(newest.html_url, "Release " + newest.tag_name));
          newer.append(", published " + day(newest.published_at) + ", has newer firmware than " +
            "this page. The site takes it when it is next published, which the release sets " +
            "off. Until then its images are on the release's page, and ");
          newer.append(link("../sink-firmware/", "the sink firmware guide"));
          newer.append(" installs them with esptool.");
          newer.hidden = false;
        }
      }
    } catch (error) {
      // No answer from GitHub: the page offers what it has.
    }
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

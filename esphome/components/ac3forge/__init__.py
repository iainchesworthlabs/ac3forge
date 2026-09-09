"""ac3forge as an ESPHome external component.

What this does and does not do, because the difference matters to anyone reading
the YAML.

DOES: pull the ac3forge ESP-IDF component into the build, and expose a small C++
object that owns a decoder and the streaming framer, so another component can
feed it bytes and get PCM back.

DOES NOT: implement a media_player or a speaker source. ESPHome's `speaker`
platform is ESP-IDF-only, so the frameworks are compatible and that is the
obvious next step - but it is a component in its own right, and shipping the
plumbing first is what lets it be built against something that already works.

The library is fetched as a GIT dependency rather than from the ESP Component
Registry, because ac3forge is not published there yet (see
.github/workflows/esp-component.yml for why that is deliberately not armed).
ESPHome's add_idf_component writes `git:`, `version:` and `path:` straight into
the generated idf_component.yml, which is exactly the form the IDF component
manager wants for a component living in a subdirectory of a repository.

    external_components:
      - source:
          type: git
          url: https://github.com/iainchesworthlabs/ac3forge
          ref: main
          path: esphome/components
        components: [ac3forge]

    ac3forge:
      version: v0.10.0-beta.1
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.const import CONF_ID

CODEOWNERS = ["@iainchesworth"]
# esp32 for add_idf_component, and because this is an ESP-IDF-only library:
# there is no Arduino path and no other platform to fall back to.
DEPENDENCIES = ["esp32"]

CONF_VERSION = "version"
CONF_BUFFER_SIZE = "buffer_size"

ac3forge_ns = cg.esphome_ns.namespace("ac3forge")
Ac3ForgeComponent = ac3forge_ns.class_("Ac3ForgeComponent", cg.Component)

# The repository the IDF component manager clones, and where the component sits
# inside it. Both are here rather than in the schema because a user overriding
# them is forking, not configuring - and a fork edits this file.
REPO = "https://github.com/iainchesworthlabs/ac3forge"
COMPONENT_PATH = "esp-idf/ac3forge"

# Bounds rather than a free integer. The floor is one syncframe plus the header
# of the next, which is what deciding where an access unit ends requires; below
# it no unit can ever be assembled and the decoder would stall reporting
# "buffer too small" forever. The ceiling is a sanity limit: this part has about
# 280 KB of internal SRAM and the decode itself peaks over 230 KB of it, so a
# buffer past 64 KB is not a configuration, it is a mistake.
MIN_BUFFER = 4160
MAX_BUFFER = 65536

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Ac3ForgeComponent),
        # A git ref: a tag for anything you intend to keep working, a branch if
        # you want to track development and are prepared for it to move.
        cv.Optional(CONF_VERSION, default="main"): cv.string_strict,
        cv.Optional(CONF_BUFFER_SIZE, default=16384): cv.All(
            cv.int_range(min=MIN_BUFFER, max=MAX_BUFFER),
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    esp32.add_idf_component(
        name="ac3forge",
        repo=REPO,
        ref=config[CONF_VERSION],
        path=COMPONENT_PATH,
    )

    var = cg.new_Pvariable(config[CONF_ID], config[CONF_BUFFER_SIZE])
    await cg.register_component(var, config)

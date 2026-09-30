#include "iclforge/sendspin/firewall.hpp"

// Linux and macOS do not gate an unlisted listener behind a one-time prompt the way Windows
// does, so there is nothing for this module to do on either.

namespace iclforge::sendspin::firewall {

bool ensure_inbound_rule(const RuleSpec& /*spec*/) { return true; }

void maybe_run_as_firewall_helper_and_exit(int /*argc*/, char** /*argv*/) {}

}  // namespace iclforge::sendspin::firewall

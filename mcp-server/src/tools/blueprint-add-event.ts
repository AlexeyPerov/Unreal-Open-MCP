import type { Tool } from "@modelcontextprotocol/sdk/types.js";

// Blueprint add event. Enable or create an overridable parent event node
// (e.g. ReceiveBeginPlay / ReceiveTick) on the event graph via the K2 schema's
// FunctionCanBePlacedAsEvent + FKismetEditorUtilities::AddDefaultEventNode.
// `path` is the Blueprint asset object path (package-path form also accepted
// — resolved in-memory first, then loaded). `name` is the parent UFunction
// name of an overridable event.
//
// The two-pronged resolution mirrors the K2 editor's own behavior:
//   - A fresh Actor event graph is pre-seeded with DISABLED ghost nodes for
//     the common events (ReceiveBeginPlay / ReceiveTick). Those ghosts are
//     INERT — the event does NOT fire until enabled. So an existing disabled
//     ghost is exactly the node this tool ENABLES: enabling the ghost IS the
//     "add event" operation (it does NOT false-succeed as a no-op).
//   - An ENABLED existing node is a real duplicate → event_already_exists.
//   - No existing node → AddDefaultEventNode mints a fresh one and the tool
//     enables it.
//
// STUB-ONLY SCOPE: this tool enables or creates the event node only. Body
// authoring (add_node / connect_pins / free-form wiring) is OUT OF SCOPE.
//
// Guards (each maps to a structured error code):
//   - the parent UFunction must be resolvable on the parent class and pass the
//     K2 schema's FunctionCanBePlacedAsEvent check (not_an_event covers BOTH a
//     name that names no parent UFunction AND a function that is not a
//     BlueprintEvent — e.g. K2_DestroyActor is BlueprintCallable but not a
//     BlueprintEvent, so placing it would seed a nonsense node + report
//     success)
//   - an already-enabled node for the event → event_already_exists
//
// Mutating: runs the full gate path (checkpoint -> add -> validate -> delta);
// `paths_hint` MUST list the Blueprint package path (e.g.
// ['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set
// gate:"off" to bypass. Chain into blueprint_get to confirm the event now
// appears in the events[] list with enabled:true, and chain into
// blueprint_compile to wire the override onto the generated class.
//
// Fidelity: greenfield. No Unity Blueprint / prefab-graph twin. Behavior
// reference (read-only): Unreal-MCP's blueprint-add-event for the
// FunctionCanBePlacedAsEvent gate + the ghost-enable resolution +
// AddDefaultEventNode.
//
// Intentional deltas vs Unreal-MCP:
//   - Canonical MCP envelope + gate summary on add.
//   - Structured error codes (not_an_event / event_already_exists /
//     no_event_graph / create_node_failed).
//   - Stub-only scope documented in the description.
//
// Route: live (POST /tools/unreal_open_mcp_blueprint_add_event). Mutating.
export const blueprintAddEvent: Tool = {
  name: "unreal_open_mcp_blueprint_add_event",
  description:
    "Enable or create an overridable parent event node (e.g. " +
    "ReceiveBeginPlay / ReceiveTick) on a Blueprint's event graph. `path` is " +
    "the Blueprint asset object path (package-path form also accepted — " +
    "resolved in-memory first, then loaded). `name` is the parent UFunction " +
    "name of an overridable event. A fresh Actor event graph is pre-seeded " +
    "with DISABLED ghost nodes for the common events — those ghosts are INERT " +
    "(the event does NOT fire) until enabled; this tool ENABLES an existing " +
    "disabled ghost (enabling the ghost IS the 'add event' op — never a " +
    "false no-op), rejects an already-enabled node, and mints + enables a " +
    "fresh node when none exists. STUB-ONLY: body authoring (add_node / " +
    "connect_pins / free-form node wiring) is OUT OF SCOPE — the event node " +
    "is empty until a later pack lands the node-authoring surface. Mutating: " +
    "runs the full gate path (checkpoint -> add -> validate -> delta); " +
    "`paths_hint` MUST list the Blueprint package path (e.g. " +
    "['/Game/Mcp/BP_Thing']) — there is no whole-project fallback, set " +
    "gate:\"off\" to bypass. Result shape: { event, enabled:true }. Error " +
    "codes: missing_parameter (path/name absent), blueprint_not_found (no " +
    "Blueprint at path), no_event_graph (Blueprint has no event graph), " +
    "not_an_event (name names no parent UFunction OR names a function that " +
    "fails the K2 schema's FunctionCanBePlacedAsEvent check — e.g. " +
    "K2_DestroyActor is BlueprintCallable but not a BlueprintEvent), " +
    "event_already_exists (an enabled node for this event is already " +
    "present), create_node_failed (AddDefaultEventNode returned null), " +
    "invalid_parameter (malformed body).",
  inputSchema: {
    type: "object",
    required: ["path", "name", "paths_hint"],
    properties: {
      path: {
        type: "string",
        description:
          "Blueprint asset object path — an object path " +
          "('/Game/Mcp/BP_Thing.BP_Thing') or a package path " +
          "('/Game/Mcp/BP_Thing'). Resolved in-memory first (a Blueprint " +
          "created this session but not yet saved), then loaded.",
      },
      name: {
        type: "string",
        description:
          "Parent event function name — an overridable BlueprintEvent on the " +
          "parent class, e.g. 'ReceiveBeginPlay', 'ReceiveTick'. " +
          "BlueprintCallable-but-not-BlueprintEvent functions (e.g. " +
          "'K2_DestroyActor') are rejected by the K2 schema's " +
          "FunctionCanBePlacedAsEvent check with not_an_event.",
      },
      paths_hint: {
        type: "array",
        items: { type: "string" },
        description:
          "Mutation scope — the Blueprint package path(s) the mutation is " +
          "scoped to, fed to the gate as the checkpoint + validate hint. " +
          "REQUIRED for mutating tools (the gate refuses an empty hint with " +
          "paths_hint_required; there is no whole-project fallback). Set " +
          "gate:\"off\" to bypass the gate and skip the hint.",
      },
      gate: {
        enum: ["enforce", "warn", "off"],
        default: "enforce",
        description:
          "Gate mode — enforce (default) runs checkpoint -> add -> validate " +
          "-> delta and hard-fails on new Errors; warn commits the mutation " +
          "but surfaces new Errors as warnings; off skips the gate entirely " +
          "(paths_hint optional).",
      },
    },
    additionalProperties: false,
  },
};

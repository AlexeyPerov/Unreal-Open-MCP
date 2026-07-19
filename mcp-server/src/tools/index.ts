import type { Tool } from "@modelcontextprotocol/sdk/types.js";
import { ping } from "./ping.js";
import { actorFind } from "./actor-find.js";
import { actorCreate } from "./actor-create.js";
import { actorModify } from "./actor-modify.js";
import { objectModify } from "./object-modify.js";
import { actorSetParent } from "./actor-set-parent.js";
import { actorDuplicate } from "./actor-duplicate.js";
import { actorDestroy } from "./actor-destroy.js";
import { actorComponentAdd } from "./actor-component-add.js";
import { actorComponentDestroy } from "./actor-component-destroy.js";
import { actorComponentGet } from "./actor-component-get.js";
import { actorComponentModify } from "./actor-component-modify.js";
import { actorComponentListAll } from "./actor-component-list-all.js";
import { levelOpen } from "./level-open.js";
import { levelSave } from "./level-save.js";
import { levelListLoaded } from "./level-list-loaded.js";
import { levelSetCurrent } from "./level-set-current.js";
import { levelUnloadSublevel } from "./level-unload-sublevel.js";
import { levelGetData } from "./level-get-data.js";
import { levelCreate } from "./level-create.js";
// P4.1 — asset read family (asset_find / asset_get_data). Read-only
// AssetRegistry queries; the default /Game scope, stable ordering, and
// offset/limit pagination make asset_find a valid P4.5 smoke candidate.
import { assetFind } from "./asset-find.js";
import { assetGetData } from "./asset-get-data.js";
// P4.2 — Content Browser CRUD (asset_create_folder / asset_copy /
// asset_move / asset_delete / asset_refresh). The four create/copy/move/
// delete tools are mutating (default gate Enforce; paths_hint required);
// asset_refresh is read-only (ScanPathsSynchronous only updates the
// in-memory registry cache, no on-disk delta for the gate to checkpoint).
import { assetCreateFolder } from "./asset-create-folder.js";
import { assetCopy } from "./asset-copy.js";
import { assetMove } from "./asset-move.js";
import { assetDelete } from "./asset-delete.js";
import { assetRefresh } from "./asset-refresh.js";
// P4.3 — material tools (material_create / material_modify /
// material_get_data). create + modify are mutating (default gate Enforce;
// paths_hint required); get_data is read-only. MIC create from a parent
// material interface + batch scalar/vector/texture parameter edits over
// UMaterialEditingLibrary.
import { materialCreate } from "./material-create.js";
import { materialModify } from "./material-modify.js";
import { materialGetData } from "./material-get-data.js";
// P4.4 — asset import (asset_import). Mutating (default gate Enforce;
// paths_hint required). Brings an absolute host filesystem source file
// (texture / static mesh / sound as the installed importers support) into a
// /Game content folder via IAssetTools::ImportAssetTasks; replace_existing /
// save flags default false.
import { assetImport } from "./asset-import.js";
// P3.6 — gate meta-tools (validate_edit / checkpoint_create / delta). The
// explicit checkpoint → mutate → delta surface; read-only (route live).
import { validateEdit } from "./validate-edit.js";
import { checkpointCreate } from "./checkpoint-create.js";
import { delta } from "./delta.js";
// P3.7 — apply_fix. Mutating tool (default gate Enforce); dry_run short-
// circuits the gate, non-dry-run applies route through the ApplyFixGateRunner
// so a FixRollback snapshot protects the asset.
import { applyFix } from "./apply-fix.js";
// P3.8 — capabilities. Local-route discovery tool: resolves in-process from
// the registered tool list + the static rule/fix catalog. No bridge hop.
import { capabilities } from "./capabilities.js";

// Tool registry. P1.7 registers the first real tool — `unreal_open_mcp_ping` —
// which the MCP server routes to the bridge's `GET /ping`. Each subsequent tool
// is defined in its own file under `src/tools/` and appended here.
//
// Every MCP tool follows the `unreal_open_mcp_*` naming convention. Tool
// definitions include: `name`, `description`, `inputSchema` (JSON Schema), and
// a handler attached at the routing layer (LiveClient → POST /tools/{name}).
//
// P2.2 adds the first real typed tool — `unreal_open_mcp_actor_find` (read-only
// actor locator). P2.3 adds the first mutating typed tool —
// `unreal_open_mcp_actor_create` (spawn in the current editor level; gate
// deferred). P2.4 adds the reflection-write pair —
// `unreal_open_mcp_actor_modify` (FProperty writes on actor(s)) and
// `unreal_open_mcp_object_modify` (FProperty writes on any UObject; gate
// deferred). P2.5 completes the actor family with three tree-structure
// mutators (`actor_set_parent`, `actor_duplicate`, `actor_destroy`) and five
// component CRUD tools (`actor_component_add`, `actor_component_destroy`,
// `actor_component_get`, `actor_component_modify`, `actor_component_list_all`);
// the read-only component tools (get, list-all) carry no gate, the mutators
// carry the forward-compat `paths_hint` + `gate` surface (deferred until P3.5).
// P2.6 adds the level lifecycle family — `level_open` (replace the editor
// world via LoadMap, with a dirty guard + ignore_dirty bypass), `level_save`
// (save in place or save-as the persistent level), `level_list_loaded`
// (read-only persistent + streaming sublevel enumeration), `level_set_current`
// (switch the actor-editing context via MakeLevelCurrent), and
// `level_unload_sublevel` (remove a streaming sublevel via
// RemoveLevelFromWorld); the read-only list tool carries no gate, the four
// mutators carry the forward-compat `paths_hint` + `gate` surface (deferred
// until P3.5). P2.7 adds the inspect + create pair — `level_get_data`
// (read-only actor roster with profile/pagination + World Partition scope
// flag) and `level_create` (new in-memory or saved-to-disk level, optionally
// template-seeded, with the same dirty guard as level_open); the read-only
// get-data tool carries no gate, the create mutator carries the forward-compat
// `paths_hint` + `gate` surface (deferred until P3.5).
// P3.6 adds the gate meta-tool family — `validate_edit` (scoped health check
// without a preceding mutation), `checkpoint_create` (capture a fingerprint
// for later delta comparison), and `delta` (compare current health vs a
// stored checkpoint). All three are read-only and route live; they
// participate in the gate workflow but bypass GatePolicy.Execute (no
// recursion). They surface the explicit checkpoint → mutate → delta contract
// agents drive when they want a manual gate pass.
// P3.7 adds `apply_fix` — the fix application workflow. Dry-run previews the
// fix (no mutation, no gate); non-dry-run applies run through the
// ApplyFixGateRunner so a FixRollback snapshot protects the asset and a
// corrupting fix is auto-reverted on failure or new errors under Enforce.
// v1 ships the single Safe provider `clear_broken_soft_reference`.
// P3.8 adds `capabilities` — the local-route discovery tool. Builds the
// capability surface (tools + rules + fixes) in-process from the static
// rule-catalog + the registered tool list, with no bridge round-trip. An
// agent calls this first to learn what is available before mutating.
// P4.1 adds the asset read family — `asset_find` (filtered AssetRegistry
// query with stable ordering + offset/limit pagination; empty filter
// defaults to /Game, never the whole registry incl. /Engine) and
// `asset_get_data` (single-asset metadata read by path-or-name, with an
// optional `paths` projection for token savings). Both are read-only (route
// live, gate-free). asset_find is a valid P4.5 smoke candidate.
// P4.2 adds the Content Browser CRUD family — `asset_create_folder`
// (idempotent MakeDirectory; refuses engine roots), `asset_copy`
// (DuplicateAsset; destination must not exist), `asset_move` (RenameAsset;
// destination must not exist; redirector note surfaced), `asset_delete`
// (DeleteAsset with a referencer guard — REFUSES with
// delete_blocked_by_referencers unless force:true), and `asset_refresh`
// (IAssetRegistry::ScanPathsSynchronous; read-only because the rescan only
// updates the in-memory cache, no on-disk delta for the gate). The four
// create/copy/move/delete tools are mutating (default gate Enforce;
// paths_hint required); asset_refresh is read-only.
// P4.3 adds the material family — `material_create` (create a
// UMaterialInstanceConstant from a parent material interface via
// UMaterialInstanceConstantFactoryNew + CreateAsset; destination must not
// exist), `material_modify` (batch scalar/vector/texture parameter overrides
// via UMaterialEditingLibrary; unknown names collected in `failed`, empty
// modify rejected with nothing_to_modify; optional save:true), and
// `material_get_data` (read-only parameter inventory + current values, shapes
// aligned with material_modify input for chaining). create + modify are
// mutating (default gate Enforce; paths_hint required); material_get_data is
// read-only.
export const ALL_TOOLS: Tool[] = [
  ping,
  actorFind,
  actorCreate,
  actorModify,
  objectModify,
  actorSetParent,
  actorDuplicate,
  actorDestroy,
  actorComponentAdd,
  actorComponentDestroy,
  actorComponentGet,
  actorComponentModify,
  actorComponentListAll,
  levelOpen,
  levelSave,
  levelListLoaded,
  levelSetCurrent,
  levelUnloadSublevel,
  levelGetData,
  levelCreate,
  validateEdit,
  checkpointCreate,
  delta,
  applyFix,
  capabilities,
  assetFind,
  assetGetData,
  assetCreateFolder,
  assetCopy,
  assetMove,
  assetDelete,
  assetRefresh,
  materialCreate,
  materialModify,
  materialGetData,
  assetImport,
];

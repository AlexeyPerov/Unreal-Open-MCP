// FProperty <-> JSON write helper for mutating tool handlers.
//
// The Unreal analog of Unity Open MCP's reflection-based field writes
// (packages/bridge/Editor/TypedTools/ReflectionScriptsObjectsTools.cs — the
// ApplyFieldPatches / ConvertValue path used by object_modify + the
// gameobject_modify jsonPatches surface). Where Unity reflects over public
// fields/properties via System.Reflection and serializes through
// SerializedObject, Unreal reflects over FProperty and writes through
// FJsonObjectConverter::JsonValueToUProperty — same role, different runtime.
//
// Behavior adapted (read-only) from Unreal-MCP's FUnrealMcpPropertyJson
// (UnrealMcpRuntime/Tools/UnrealMcpPropertyJson.cpp): the actor/scene-component
// transform keys are NOT single FProperties, so they are special-cased to the
// transform APIs and handled TERMINALLY (a parse failure records an error and
// moves on — it never falls through to the generic FProperty path, which would
// either count an empty-object no-op as applied or emit a misleading
// "unknown property" for the pseudo-keys). The generic path resolves a
// reflected FProperty by case-insensitive name, rejects read-only properties
// explicitly (a clear error, not a silent no-op), and hands the JSON value to
// FJsonObjectConverter.
//
// P2.4 scope: a bounded write surface. Only ApplyProperties ships — the read
// side (SerializeObject / scoped reads) lands with the level/object get-data
// tools in a later phase. Supported value kinds in P2: bool, int/byte, float,
// FString, FVector ({x,y,z}), FRotator ({pitch,yaw,roll}), FLinearColor
// ({r,g,b,a}), enums (by name), plus the actor/scene-component transform
// shortcuts — everything FJsonObjectConverter::JsonValueToUProperty handles
// for these property types.
//
// Every function here runs ON THE GAME THREAD (it touches the UObject graph,
// the transaction buffer, and the editor edit-protocol hooks). The HTTP server
// marshals every tool dispatch through the GameThreadDispatcher before a
// handler is invoked, so callers can use these helpers freely from inside a
// FUnrealOpenMcpToolHandler.
//
// Fidelity (P2.4): adapt — port Unreal-MCP FUnrealMcpPropertyJson::ApplyProperties
// (transform terminal handling + FProperty writable gate + FJsonObjectConverter
// write + WITH_EDITOR Modify/PreEditChange/PostEditChange bracket).
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"   // TSharedPtr<FJsonObject> appears by value in the signature below.

class UObject;

/**
 * FProperty <-> JSON write helpers shared by mutating tool handlers
 * (actor_modify, object_modify, and later component_modify). Lives in a
 * self-contained header so the property-write contract is identical across
 * the mutate family. Writes are reflected — none of them spawn/destroy UObjects.
 */
namespace FUnrealOpenMcpPropertyJson
{
	/**
	 * Apply each field of @p Properties to @p Object via FProperty reflection.
	 * Actor transform keys (`location`/`rotation`/`scale`) and scene-component
	 * relative-transform keys (`relativeLocation`/`relativeRotation`/
	 * `relativeScale3D`, alias `relativeScale`) are special-cased to the
	 * SetActor* / SetRelative* APIs (an actor/component transform is not a
	 * single writable FProperty); everything else is set by name through
	 * FJsonObjectConverter::JsonValueToUProperty.
	 *
	 * Returns the number of fields successfully applied; appends a one-line
	 * reason per failed field to @p OutErrors (unknown property name, read-only
	 * property, value-conversion failure, malformed transform object). A partial
	 * success is the norm — one bad field does not abort the batch.
	 *
	 * Snapshots the object into the transaction buffer (Modify) and runs the
	 * editor edit protocol (PreEditChange/PostEditChange) so the writes are
	 * Undo-able when wrapped in an FScopedTransaction by the caller. Marks the
	 * object's package dirty when at least one field changed.
	 */
	UNREALOPENMCPEDITOR_API int32 ApplyProperties(
		UObject* Object,
		const TSharedPtr<FJsonObject>& Properties,
		TArray<FString>& OutErrors);
}

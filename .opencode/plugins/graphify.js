// graphify OpenCode plugin
// Prepends a one-shot knowledge-graph hint to the first shell command when the graph exists.
// Dual-compatible: V1 calls the default export's server(); V2 reads id + setup(ctx).
// The hint is POSIX- and PowerShell-safe: single-quoted (literal backticks) with a `;` separator.
import { existsSync } from "fs";
import { join } from "path";

const REMINDER =
  "echo '[graphify] knowledge graph at graphify-out/. For focused questions, run `graphify query \"<question>\"` (scoped subgraph, usually much smaller than GRAPH_REPORT.md) instead of grepping raw files. Read GRAPH_REPORT.md only for broad architecture context.' ; ";

// Returns a one-shot remind function that prepends the hint to the first shell command.
function makeReminder(directory) {
  let reminded = false;
  return (tool, args) => {
    if (reminded) return;
    if (tool !== "shell" && tool !== "bash") return;
    if (!args || typeof args.command !== "string") return;
    if (!existsSync(join(directory, "graphify-out", "graph.json"))) return;
    args.command = REMINDER + args.command;
    reminded = true;
  };
}

// V1 (OpenCode 1.x) implementation.
export const GraphifyPlugin = async ({ directory }) => {
  const remind = makeReminder(directory);
  return {
    "tool.execute.before": async (input, output) => {
      remind(input.tool, output.args);
    },
  };
};

// V2 (OpenCode 2.x) implementation.
async function setup(ctx) {
  // V1 also invokes setup() with a V1-shaped context that has no tool domain.
  if (!ctx?.tool || typeof ctx.tool.hook !== "function") return;
  const remind = makeReminder(ctx.location.directory);
  await ctx.tool.hook("execute.before", (event) => {
    remind(event.tool, event.input);
  });
}

export default {
  id: "graphify",
  server: GraphifyPlugin,
  setup,
};

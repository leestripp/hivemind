// js/state.js — shared mutable UI state + node type registry.

export const NODE_DEFS = {
  input:       { color: '#a3be8c', label: 'Input',       in: [],        out: ['out'] },
  prompt:      { color: '#b48ead', label: 'Prompt',      in: ['in'],     out: ['out'] },
  agent:       { color: '#88c0d0', label: 'AI Agent',    in: ['in'],     out: ['out'] },
  merge:       { color: '#ebcb8b', label: 'Merge',       in: ['in_a','in_b','in_c'], out: ['out'] },
  output:      { color: '#d08770', label: 'Output',      in: ['in'],     out: [] },
  log:         { color: '#6c7280', label: 'Log',         in: ['in'],     out: [] },
  loop_start:  { color: '#5e81ac', label: 'Loop Start',  in: ['in'],     out: ['out','loop_count'] },
  loop_end:    { color: '#5e81ac', label: 'Loop End',    in: ['in','loop_count'], out: ['out'] },
  agent_loop_start: { color: '#bf616a', label: 'Agent Loop Start', in: ['in'], out: ['out','loop_count'] },
  agent_loop_end:   { color: '#bf616a', label: 'Agent Loop End (judge)', in: ['in','prompt_in','loop_count'], out: ['out'] },
};

export const S = {
  nodes: [],
  edges: [],
  selected: null,
  dragging: null,
  connecting: null,
  selectedEdge: null,
  running: false,
  // Canvas transform (pan/zoom). Node positions are stored in canvas coordinates.
  scale: 1,
  panX: 0,
  panY: 0,
  panning: null,
  spaceHeld: false,
  menuPos: null,
};

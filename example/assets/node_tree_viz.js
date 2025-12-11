/**
 * Node Tree Visualization
 *
 * Displays the scsynth node tree with color-coded node types:
 * - Groups (orange), FX (magenta), Samples (cyan), Synths (green)
 *
 * Two visualization modes:
 * - 2D: Vertical tree layout using d3-hierarchy (root at top)
 * - 3D: Radial force graph with bloom effects (lazy-loaded)
 */

// Color palette - distinct colors for each node type
const COLORS = {
  group: '#ff8c00',      // Orange for groups
  fx: '#ff00ff',         // Magenta for FX
  sample: '#00ffff',     // Cyan for samples
  synth: '#00ff88',      // Green for synths
  background: '#000000',
  link: '#ff8c00'
};

// Determine node type from defName
function getNodeType(node) {
  const data = node.data || node;

  if (data.isGroup) return 'group';

  const defName = data.defName || '';

  // Sample players
  if (defName.includes('stereo_player') || defName.includes('mono_player')) {
    return 'sample';
  }

  // FX synths
  if (defName.includes('-fx_') || defName.includes('-fx-')) {
    return 'fx';
  }

  // Regular synths
  return 'synth';
}

// Determine node color based on type
function getNodeColor(node) {
  const data = node.data || node;

  // Root group (ID 0) - slightly brighter orange
  if (data.id === 0) return '#ffaa00';

  const type = getNodeType(node);
  return COLORS[type];
}

export class NodeTreeViz {
  constructor(container2d, container3d, supersonic) {
    this.container2d = container2d;
    this.container3d = container3d;
    this.supersonic = supersonic;
    this.graph3d = null;
    this.svg = null;
    this.lastVersion = -1;
    this.animationId = null;
    this.is3D = false;

    // Track current state
    this.currentNodes = new Map();
    this.currentLinks = new Map();
  }

  async init() {
    // Use 2D container for initial size (it's visible on init)
    const width = this.container2d.clientWidth || 400;
    const height = this.container2d.clientHeight || 180;

    // Create SVG for d3 tree (2D mode) - init first since we start in 2D
    this.initD3Tree(width, height);

    // Start polling for tree updates
    this.startPolling();
  }

  /**
   * Lazily initialize 3D graph when first switching to 3D mode
   */
  async init3D() {
    if (this.graph3d) return;

    // Get parent container dimensions (the wrapper div)
    const parent = this.container3d.parentElement;
    const width = parent.clientWidth || 400;
    const height = parent.clientHeight || 180;

    // Create the 3D force graph
    this.graph3d = ForceGraph3D()(this.container3d)
      .width(width)
      .height(height)
      .backgroundColor(COLORS.background)
      .nodeLabel(node => `${node.defName} (${node.id})`)
      .nodeColor(getNodeColor)
      .nodeVal(node => node.isGroup ? 8 : 3)
      .nodeOpacity(0.9)
      .linkColor(() => 'rgba(255, 140, 0, 0.4)')
      .linkWidth(1)
      .linkOpacity(0.6)
      .linkDirectionalParticles(2)
      .linkDirectionalParticleWidth(1.5)
      .linkDirectionalParticleSpeed(0.006)
      .linkDirectionalParticleColor(() => '#ff8c00')
      .d3AlphaDecay(0.02)
      .d3VelocityDecay(0.4)
      .warmupTicks(0)
      .cooldownTicks(0)
      .dagMode('radialout')
      .dagLevelDistance(50)
      .enableNavigationControls(true);

    // Add bloom effect to 3D
    await this.addBloomEffect();

    // Fit 3D graph to view
    setTimeout(() => {
      this.graph3d.zoomToFit(400, 50);
    }, 500);
  }

  initD3Tree(width, height) {
    // Clear any existing SVG
    this.container2d.innerHTML = '';

    // Create SVG - use 100% width to fill container
    this.svg = d3.select(this.container2d)
      .append('svg')
      .attr('width', '100%')
      .attr('height', '100%')
      .style('background', COLORS.background);

    // Create a group for zooming/panning
    this.svgGroup = this.svg.append('g');

    // Create groups for links and nodes (links behind nodes)
    this.linksGroup = this.svgGroup.append('g').attr('class', 'links');
    this.nodesGroup = this.svgGroup.append('g').attr('class', 'nodes');
  }

  /**
   * Convert flat node list to d3 hierarchy
   */
  buildHierarchy(nodes) {
    if (nodes.length === 0) {
      return null;
    }

    // Create a map for quick lookup
    const nodeMap = new Map();
    nodes.forEach(n => {
      nodeMap.set(n.id, { ...n, children: [] });
    });

    // Find root and build tree
    let root = null;
    nodeMap.forEach((node, id) => {
      if (node.parentId === -1 || !nodeMap.has(node.parentId)) {
        // This is a root node
        if (!root || id === 0) {
          root = node;
        }
      } else {
        // Add to parent's children
        const parent = nodeMap.get(node.parentId);
        if (parent) {
          parent.children.push(node);
        }
      }
    });

    // If no root found, use first node
    if (!root && nodes.length > 0) {
      root = nodeMap.get(nodes[0].id);
    }

    return root;
  }

  /**
   * Update the d3 tree visualization
   * Root at top, children below - vertical tree layout
   */
  updateD3Tree(nodes) {
    if (nodes.length === 0) {
      this.linksGroup.selectAll('*').remove();
      this.nodesGroup.selectAll('*').remove();
      return;
    }

    if (!this.nodePositions) this.nodePositions = new Map();

    // Build hierarchy
    const rootData = this.buildHierarchy(nodes);
    if (!rootData) return;

    const root = d3.hierarchy(rootData);
    const treeNodes = root.descendants();

    // Use d3.tree for vertical layout - root at top
    const treeLayout = d3.tree().nodeSize([25, 40]);
    treeLayout(root);

    // Viscosity - groups very slow, synths fast
    const GROUP_VISCOSITY = 0.03;
    const SYNTH_VISCOSITY = 0.3;

    // Update positions with viscosity
    // For vertical tree: d.x = horizontal spread, d.y = vertical depth
    treeNodes.forEach(d => {
      const id = d.data.id;
      const targetX = d.x;           // spread -> horizontal
      const targetY = d.y + 20;      // depth -> vertical (root at top)

      if (this.nodePositions.has(id)) {
        const pos = this.nodePositions.get(id);
        const viscosity = d.data.isGroup ? GROUP_VISCOSITY : SYNTH_VISCOSITY;
        pos.x += (targetX - pos.x) * viscosity;
        pos.y += (targetY - pos.y) * viscosity;
      } else {
        this.nodePositions.set(id, { x: targetX, y: targetY });
      }
    });

    // Remove deleted nodes
    const currentIds = new Set(treeNodes.map(d => d.data.id));
    for (const [id] of this.nodePositions) {
      if (!currentIds.has(id)) this.nodePositions.delete(id);
    }

    // Build node data with current positions
    const nodeData = treeNodes.map(d => {
      const pos = this.nodePositions.get(d.data.id);
      return {
        key: d.data.id,
        x: pos.x,
        y: pos.y,
        data: d.data,
        isGroup: d.data.isGroup,
        node: d
      };
    });

    // Auto-fit viewport - get current container dimensions
    const width = this.container2d.clientWidth || 400;
    const height = this.container2d.clientHeight || 180;
    const padding = 20;

    // Detect container resize - reset view immediately on any size change
    const containerResized = this.lastContainerWidth && width !== this.lastContainerWidth;
    this.lastContainerWidth = width;

    if (nodeData.length > 0) {
      const minX = Math.min(...nodeData.map(d => d.x));
      const maxX = Math.max(...nodeData.map(d => d.x));
      const minY = Math.min(...nodeData.map(d => d.y));
      const maxY = Math.max(...nodeData.map(d => d.y));

      const treeWidth = Math.max(maxX - minX, 1) + 40;
      const treeHeight = Math.max(maxY - minY, 1) + 40;

      const scaleX = (width - padding * 2) / treeWidth;
      const scaleY = (height - padding * 2) / treeHeight;
      const targetScale = Math.min(scaleX, scaleY, 1.5);

      // Center tree horizontally: translate to center, then offset by tree center
      const treeCenterX = (minX + maxX) / 2;
      const targetTransX = width / 2 - treeCenterX * targetScale;
      const topPadding = 45; // Extra space for stats label at top
      const targetTransY = topPadding - minY * targetScale;

      // On container resize, snap immediately; otherwise use viscosity
      if (!this.viewTransform || containerResized) {
        this.viewTransform = { scale: targetScale, x: targetTransX, y: targetTransY };
      } else {
        const VIEW_VISCOSITY = 0.03;
        this.viewTransform.scale += (targetScale - this.viewTransform.scale) * VIEW_VISCOSITY;
        this.viewTransform.x += (targetTransX - this.viewTransform.x) * VIEW_VISCOSITY;
        this.viewTransform.y += (targetTransY - this.viewTransform.y) * VIEW_VISCOSITY;
      }

      this.svgGroup.attr('transform', `translate(${this.viewTransform.x}, ${this.viewTransform.y}) scale(${this.viewTransform.scale})`);
    }

    // Build link data
    const linkData = [];
    treeNodes.forEach(d => {
      if (d.parent) {
        const sourcePos = this.nodePositions.get(d.parent.data.id);
        const targetPos = this.nodePositions.get(d.data.id);
        if (sourcePos && targetPos) {
          linkData.push({
            key: `${d.parent.data.id}-${d.data.id}`,
            x1: sourcePos.x,
            y1: sourcePos.y,
            x2: targetPos.x,
            y2: targetPos.y
          });
        }
      }
    });

    // Update links - vertical curves (top to bottom)
    const links = this.linksGroup.selectAll('path')
      .data(linkData, d => d.key);

    links.exit().remove();

    links.enter()
      .append('path')
      .attr('fill', 'none')
      .attr('stroke', COLORS.link)
      .attr('stroke-opacity', 0.6)
      .attr('stroke-width', 1.5)
      .merge(links)
      .attr('d', d => {
        // Vertical curve - top to bottom
        const midY = (d.y1 + d.y2) / 2;
        return `M${d.x1},${d.y1} C${d.x1},${midY} ${d.x2},${midY} ${d.x2},${d.y2}`;
      });

    // Update nodes
    const nodeGroups = this.nodesGroup.selectAll('g.node')
      .data(nodeData, d => d.key);

    nodeGroups.exit().remove();

    const nodeEnter = nodeGroups.enter()
      .append('g')
      .attr('class', 'node');

    nodeEnter.append('circle')
      .attr('stroke-width', 2)
      .style('filter', 'drop-shadow(0 0 3px currentColor)');

    // Tooltip only (no visible text labels)
    nodeEnter.append('title');

    const allNodes = nodeEnter.merge(nodeGroups);

    allNodes.attr('transform', d => `translate(${d.x},${d.y})`);

    allNodes.select('circle')
      .attr('r', d => d.isGroup ? 6 : 4)
      .attr('fill', d => getNodeColor(d.data))
      .attr('stroke', d => getNodeColor(d.data));

    allNodes.select('title')
      .text(d => `${d.data.defName} (${d.data.id})`);
  }

  toggleDimensions() {
    this.is3D = !this.is3D;

    this.container2d.style.display = this.is3D ? 'none' : 'block';
    this.container3d.style.display = this.is3D ? 'block' : 'none';

    if (this.is3D) {
      // Lazily init 3D graph on first switch
      this.init3D().then(() => {
        // Update 3D graph
        const nodes = Array.from(this.currentNodes.values());
        const links = Array.from(this.currentLinks.values());
        this.graph3d.graphData({ nodes, links });
        this.graph3d.d3ReheatSimulation();
      });
    } else {
      // Update 2D tree
      const nodes = Array.from(this.currentNodes.values());
      this.updateD3Tree(nodes);
    }

    return this.is3D ? '3D' : '2D';
  }

  async addBloomEffect() {
    try {
      const THREE = await import('https://cdn.jsdelivr.net/npm/three@0.181.0/build/three.module.js');
      const { UnrealBloomPass } = await import('https://cdn.jsdelivr.net/npm/three@0.181.0/examples/jsm/postprocessing/UnrealBloomPass.js');

      const bloomPass = new UnrealBloomPass(
        new THREE.Vector2(this.container3d.clientWidth, this.container3d.clientHeight),
        1.5, 0.4, 0.1
      );

      const composer = this.graph3d.postProcessingComposer();
      composer.addPass(bloomPass);
    } catch (e) {
      console.warn('Could not add bloom effect:', e);
    }
  }

  startPolling() {
    const poll = () => {
      this.update();
      this.animationId = requestAnimationFrame(poll);
    };
    poll();
  }

  stopPolling() {
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
  }

  update() {
    if (!this.supersonic?.initialized) return;

    const tree = this.supersonic.getTree();
    if (tree.version === this.lastVersion) return;
    this.lastVersion = tree.version;

    // Build node map
    const newNodeIds = new Set(tree.nodes.map(n => n.id));

    // Remove old nodes
    for (const [id] of this.currentNodes) {
      if (!newNodeIds.has(id)) {
        this.currentNodes.delete(id);
      }
    }

    // Remove old links
    const newLinkKeys = new Set();
    tree.nodes.forEach(n => {
      if (n.parentId !== -1) {
        newLinkKeys.add(`${n.parentId}-${n.id}`);
      }
    });
    for (const [key] of this.currentLinks) {
      if (!newLinkKeys.has(key)) {
        this.currentLinks.delete(key);
      }
    }

    // Add/update nodes
    for (const n of tree.nodes) {
      if (!this.currentNodes.has(n.id)) {
        this.currentNodes.set(n.id, {
          id: n.id,
          defName: n.defName,
          isGroup: n.isGroup,
          parentId: n.parentId,
          x: (Math.random() - 0.5) * 50,
          y: (Math.random() - 0.5) * 50,
          z: (Math.random() - 0.5) * 50
        });
      } else {
        const node = this.currentNodes.get(n.id);
        node.defName = n.defName;
        node.isGroup = n.isGroup;
        node.parentId = n.parentId;
      }
    }

    // Add links
    for (const n of tree.nodes) {
      if (n.parentId !== -1) {
        const key = `${n.parentId}-${n.id}`;
        if (!this.currentLinks.has(key) &&
            this.currentNodes.has(n.parentId) &&
            this.currentNodes.has(n.id)) {
          this.currentLinks.set(key, {
            source: n.parentId,
            target: n.id
          });
        }
      }
    }

    // Update visualization
    const nodes = Array.from(this.currentNodes.values());
    const links = Array.from(this.currentLinks.values());

    if (this.is3D && this.graph3d) {
      this.graph3d.graphData({ nodes, links });
    } else if (!this.is3D) {
      this.updateD3Tree(nodes);
    }

    // Update stats
    this.updateStats(nodes);
  }

  updateStats(nodes) {
    let groups = 0, fx = 0, samples = 0, synths = 0;

    for (const node of nodes) {
      const type = getNodeType(node);
      switch (type) {
        case 'group': groups++; break;
        case 'fx': fx++; break;
        case 'sample': samples++; break;
        case 'synth': synths++; break;
      }
    }

    const statGroups = document.getElementById('stat-groups');
    const statFx = document.getElementById('stat-fx');
    const statSamples = document.getElementById('stat-samples');
    const statSynths = document.getElementById('stat-synths');

    if (statGroups) statGroups.textContent = groups;
    if (statFx) statFx.textContent = fx;
    if (statSamples) statSamples.textContent = samples;
    if (statSynths) statSynths.textContent = synths;
  }

  destroy() {
    this.stopPolling();
    if (this.graph3d) {
      this.graph3d._destructor && this.graph3d._destructor();
      this.graph3d = null;
    }
    if (this.svg) {
      this.svg.remove();
      this.svg = null;
    }
  }
}

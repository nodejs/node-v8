// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

"use strict";

class GraphMultiView extends View {
  createViewElement() {
    const pane = document.createElement('div');
    pane.setAttribute('id', "multiview");
    return pane;
  }

  constructor(id, selectionBroker, sourceResolver) {
    super(id);
    const view = this;
    view.sourceResolver = sourceResolver;
    view.selectionBroker = selectionBroker;
    this.graph = new GraphView(id, selectionBroker,
      (phaseName) => view.displayPhaseByName(phaseName));
    this.schedule = new ScheduleView(id, selectionBroker);


    function handleSearch(e) {
      if (this.currentPhaseView) {
        this.currentPhaseView.searchInputAction(this.currentPhaseView, this)
      }
    }
    d3.select("#search-input").on("keyup", handleSearch);
    d3.select("#search-input").attr("value", window.sessionStorage.getItem("lastSearch") || "");
    this.selectMenu = document.getElementById('display-selector');
  }

  initializeSelect() {
    const view = this;
    view.selectMenu.innerHTML = '';
    view.sourceResolver.forEachPhase((phase) => {
      const optionElement = document.createElement("option");
      optionElement.text = phase.name;
      view.selectMenu.add(optionElement);
    });
    view.selectMenu.onchange = function () {
      window.sessionStorage.setItem("lastSelectedPhase", this.selectedIndex);
      view.displayPhase(view.sourceResolver.getPhase(this.selectedIndex));
    }
  }

  show(data, rememberedSelection) {
    super.show(data, rememberedSelection);
    this.initializeSelect();
    const lastPhaseIndex = +window.sessionStorage.getItem("lastSelectedPhase");
    const initialPhaseIndex = this.sourceResolver.repairPhaseId(lastPhaseIndex);
    this.selectMenu.selectedIndex = initialPhaseIndex;
    this.displayPhase(this.sourceResolver.getPhase(initialPhaseIndex));
  }

  initializeContent() {}

  displayPhase(phase) {
    if (phase.type == 'graph') {
      this.displayPhaseView(this.graph, phase.data);
    } else if (phase.type == 'schedule') {
      this.displayPhaseView(this.schedule, phase);
    }
  }

  displayPhaseView(view, data) {
    const rememberedSelection = this.hideCurrentPhase();
    view.show(data, rememberedSelection);
    d3.select("#middle").classed("scrollable", view.isScrollable());
    this.currentPhaseView = view;
  }

  displayPhaseByName(phaseName) {
    const phaseId = this.sourceResolver.getPhaseIdByName(phaseName);
    this.selectMenu.selectedIndex = phaseId - 1;
    this.displayPhase(this.sourceResolver.getPhase(phaseId));
  }

  hideCurrentPhase() {
    let rememberedSelection = null;
    if (this.currentPhaseView != null) {
      rememberedSelection = this.currentPhaseView.detachSelection();
      this.currentPhaseView.hide();
      this.currentPhaseView = null;
    }
    return rememberedSelection;
  }

  onresize() {
    if (this.graph) this.graph.fitGraphViewToWindow();
  }

  deleteContent() {
    this.hideCurrentPhase();
  }
}

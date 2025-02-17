/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import { createSelector } from "reselect";

import { isGeneratedId } from "devtools/client/shared/source-map-loader/index";
import { makeBreakpointId } from "../utils/breakpoint";

// This method is only used from the main test helper
export function getBreakpointsMap(state) {
  return state.breakpoints.breakpoints;
}

export const getBreakpointsList = createSelector(
  state => state.breakpoints.breakpoints,
  breakpoints => Object.values(breakpoints)
);

export function getBreakpointCount(state) {
  return getBreakpointsList(state).length;
}

export function getBreakpoint(state, location) {
  if (!location) {
    return undefined;
  }

  const breakpoints = getBreakpointsMap(state);
  return breakpoints[makeBreakpointId(location)];
}

/**
 * Gets the breakpoints on a line or within a range of lines
 * @param {Object} state
 * @param {Number} sourceId
 * @param {Number|Object} lines - line or an object with a start and end range of lines
 * @returns {Array} breakpoints
 */
export function getBreakpointsForSource(state, sourceId, lines) {
  if (!sourceId) {
    return [];
  }

  const isGeneratedSource = isGeneratedId(sourceId);
  const breakpoints = getBreakpointsList(state);
  return breakpoints.filter(bp => {
    const location = isGeneratedSource ? bp.generatedLocation : bp.location;

    if (lines) {
      const isOnLineOrWithinRange =
        typeof lines == "number"
          ? location.line == lines
          : location.line >= lines.start.line &&
            location.line <= lines.end.line;
      return location.source.id === sourceId && isOnLineOrWithinRange;
    }
    return location.source.id === sourceId;
  });
}

export function getHiddenBreakpoint(state) {
  const breakpoints = getBreakpointsList(state);
  return breakpoints.find(bp => bp.options.hidden);
}

export function hasLogpoint(state, location) {
  const breakpoint = getBreakpoint(state, location);
  return breakpoint?.options.logValue;
}

export function getXHRBreakpoints(state) {
  return state.breakpoints.xhrBreakpoints;
}

export const shouldPauseOnAnyXHR = createSelector(
  getXHRBreakpoints,
  xhrBreakpoints => {
    const emptyBp = xhrBreakpoints.find(({ path }) => path.length === 0);
    if (!emptyBp) {
      return false;
    }

    return !emptyBp.disabled;
  }
);

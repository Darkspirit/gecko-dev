/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import { getFrames, getSymbols, getCurrentThread } from "../../selectors";

import { findClosestFunction } from "../../utils/ast";

function mapDisplayName(frame, { getState }) {
  if (frame.isOriginal) {
    return frame;
  }

  const symbols = getSymbols(getState(), frame.location);

  if (!symbols || !symbols.functions) {
    return frame;
  }

  const originalFunction = findClosestFunction(symbols, frame.location);

  if (!originalFunction) {
    return frame;
  }

  const originalDisplayName = originalFunction.name;
  return { ...frame, originalDisplayName };
}

export function mapDisplayNames() {
  return function ({ dispatch, getState }) {
    const thread = getCurrentThread(getState());
    const frames = getFrames(getState(), thread);

    if (!frames) {
      return;
    }

    const mappedFrames = frames.map(frame =>
      mapDisplayName(frame, { getState })
    );

    dispatch({
      type: "MAP_FRAME_DISPLAY_NAMES",
      thread,
      frames: mappedFrames,
    });
  };
}

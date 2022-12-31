/* Communication module for Android terminals.  -*- c-file-style: "GNU" -*-

Copyright (C) 2023 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

package org.gnu.emacs;

import android.view.View;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.Region;
import android.graphics.Paint;
import android.util.Log;

import android.os.Build;

/* This is an Android view which has a back and front buffer.  When
   swapBuffers is called, the back buffer is swapped to the front
   buffer, and any damage is invalidated.  frontBitmap and backBitmap
   are modified and used both from the UI and the Emacs thread.  As a
   result, there is a lock held during all drawing operations.

   It is also a ViewGroup, as it also lays out children.  */

public class EmacsView extends ViewGroup
{
  public static final String TAG = "EmacsView";

  /* The associated EmacsWindow.  */
  public EmacsWindow window;

  /* The buffer bitmap.  */
  public Bitmap bitmap;

  /* The associated canvases.  */
  public Canvas canvas;

  /* The damage region.  */
  public Region damageRegion;

  /* The paint.  */
  public Paint paint;

  /* The associated surface view.  */
  private EmacsSurfaceView surfaceView;

  public
  EmacsView (EmacsWindow window)
  {
    super (EmacsService.SERVICE);

    this.window = window;
    this.damageRegion = new Region ();
    this.paint = new Paint ();

    /* Create the surface view.  */
    this.surfaceView = new EmacsSurfaceView (this);

    setFocusable (FOCUSABLE);
    addView (this.surfaceView);
  }

  @Override
  protected void
  onMeasure (int widthMeasureSpec, int heightMeasureSpec)
  {
    Rect measurements;
    int width, height;

    /* Return the width and height of the window regardless of what
       the parent says.  */
    measurements = window.getGeometry ();

    width = measurements.width ();
    height = measurements.height ();

    /* Now apply any extra requirements in widthMeasureSpec and
       heightMeasureSpec.  */

    if (MeasureSpec.getMode (widthMeasureSpec) == MeasureSpec.EXACTLY)
      width = MeasureSpec.getSize (widthMeasureSpec);
    else if (MeasureSpec.getMode (widthMeasureSpec) == MeasureSpec.AT_MOST
	     && width > MeasureSpec.getSize (widthMeasureSpec))
      width = MeasureSpec.getSize (widthMeasureSpec);

    if (MeasureSpec.getMode (heightMeasureSpec) == MeasureSpec.EXACTLY)
      height = MeasureSpec.getSize (heightMeasureSpec);
    else if (MeasureSpec.getMode (heightMeasureSpec) == MeasureSpec.AT_MOST
	     && height > MeasureSpec.getSize (heightMeasureSpec))
      height = MeasureSpec.getSize (heightMeasureSpec);

    super.setMeasuredDimension (width, height);
  }

  @Override
  protected void
  onLayout (boolean changed, int left, int top, int right,
	    int bottom)
  {
    int count, i;
    View child;
    Rect windowRect;

    if (changed)
      {
	window.viewLayout (left, top, right, bottom);

	/* Recreate the front and back buffer bitmaps.  */
	bitmap
	  = Bitmap.createBitmap (right - left, bottom - top,
				 Bitmap.Config.ARGB_8888);

	/* And canvases.  */
	canvas = new Canvas (bitmap);
      }

    count = getChildCount ();

    for (i = 0; i < count; ++i)
      {
	child = getChildAt (i);

	if (child == surfaceView)
	  /* The child is the surface view, so give it the entire
	     view.  */
	  child.layout (left, top, right, bottom);
	else if (child.getVisibility () != GONE)
	  {
	    if (!(child instanceof EmacsView))
	      continue;

	    /* What to do: lay out the view precisely according to its
	       window rect.  */
	    windowRect = ((EmacsView) child).window.getGeometry ();
	    child.layout (windowRect.left, windowRect.top,
			  windowRect.right, windowRect.bottom);
	  }
      }
  }

  public void
  damageRect (Rect damageRect)
  {
    damageRegion.union (damageRect);
  }

  public void
  swapBuffers ()
  {
    Bitmap back;
    Canvas canvas;
    Rect damageRect;

    if (damageRegion.isEmpty ())
      return;

    if (!surfaceView.isCreated ())
      return;

    if (bitmap == null)
      return;

    /* Lock the canvas with the specified damage.  */
    damageRect = damageRegion.getBounds ();
    canvas = surfaceView.lockCanvas (damageRect);

    /* Return if locking the canvas failed.  */
    if (canvas == null)
      return;

    /* Copy from the back buffer to the canvas.  */
    canvas.drawBitmap (bitmap, damageRect, damageRect, paint);

    /* Unlock the canvas and clear the damage.  */
    surfaceView.unlockCanvasAndPost (canvas);
    damageRegion.setEmpty ();
  }

  @Override
  public boolean
  onKeyDown (int keyCode, KeyEvent event)
  {
    window.onKeyDown (keyCode, event);
    return true;
  }

  @Override
  public boolean
  onKeyUp (int keyCode, KeyEvent event)
  {
    window.onKeyUp (keyCode, event);
    return true;
  }
};

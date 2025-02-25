package com.frank.camerafilter.factory;

import android.content.Context;

import com.frank.camerafilter.filter.advance.BeautyBlurFilter;
import com.frank.camerafilter.filter.advance.BeautyColorInvertFilter;
import com.frank.camerafilter.filter.advance.BeautyHueFilter;
import com.frank.camerafilter.filter.advance.BeautySketchFilter;
import com.frank.camerafilter.filter.BaseFilter;
import com.frank.camerafilter.filter.advance.BeautyWhiteBalanceFilter;

public class BeautyFilterFactory {

    private static BeautyFilterType filterType = BeautyFilterType.NONE;

    public static BaseFilter getFilter(BeautyFilterType type, Context context) {
        filterType = type;
        switch (type) {
            case SKETCH:
                return new BeautySketchFilter(context);
            case BLUR:
                return new BeautyBlurFilter(context);
            case COLOR_INVERT:
                return new BeautyColorInvertFilter(context);
            case HUE:
                return new BeautyHueFilter(context);
            case WHITE_BALANCE:
                return new BeautyWhiteBalanceFilter(context);
            default:
                return null;
        }
    }

    public static BeautyFilterType getFilterType() {
        return filterType;
    }

}

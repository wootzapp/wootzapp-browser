// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.wootz_authoring;

import android.os.Bundle;
import android.view.Menu;
import android.view.MenuInflater;

import androidx.preference.Preference;

import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSupplierImpl;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;
import org.chromium.chrome.browser.settings.search.ChromeBaseSearchIndexProvider;
import org.chromium.components.browser_ui.settings.EmbeddableSettingsPage;
import org.chromium.components.browser_ui.settings.SettingsFragment;
import org.chromium.components.browser_ui.settings.SettingsUtils;

/** Top-level settings page for Wootz authoring and replay controls. */
@NullMarked
public class WootzAuthoringSettingsFragment extends ChromeBaseSettingsFragment
        implements EmbeddableSettingsPage {
    public static final String ARG_CURRENT_TAB_URL = "current_tab_url";

    private static final String PREF_CURRENT_TAB_URL = "wootz_authoring_current_tab_url";
    private final ObservableSupplierImpl<String> mPageTitle = new ObservableSupplierImpl<>();

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        setHasOptionsMenu(true);
        mPageTitle.set(getString(R.string.wootz_authoring_settings_title));
        SettingsUtils.addPreferencesFromResource(this, R.xml.wootz_authoring_preferences);

        Preference currentTabUrlPreference = findPreference(PREF_CURRENT_TAB_URL);
        if (currentTabUrlPreference != null) {
            Bundle args = getArguments();
            String currentTabUrl =
                    args == null ? null : args.getString(ARG_CURRENT_TAB_URL, /* defaultValue= */ "");
            if (currentTabUrl == null || currentTabUrl.isEmpty()) {
                currentTabUrlPreference.setSummary(R.string.wootz_authoring_current_tab_url_empty);
            } else {
                currentTabUrlPreference.setSummary(currentTabUrl);
            }
        }
    }

    @Override
    public void onCreateOptionsMenu(Menu menu, MenuInflater inflater) {
        menu.clear();
    }

    @Override
    public MonotonicObservableSupplier<String> getPageTitle() {
        return mPageTitle;
    }

    @Override
    public @SettingsFragment.AnimationType int getAnimationType() {
        return SettingsFragment.AnimationType.PROPERTY;
    }

    @Override
    public @Nullable String getMainMenuKey() {
        return "wootz_authoring";
    }

    public static final ChromeBaseSearchIndexProvider SEARCH_INDEX_DATA_PROVIDER =
            new ChromeBaseSearchIndexProvider(
                    WootzAuthoringSettingsFragment.class.getName(),
                    R.xml.wootz_authoring_preferences);
}

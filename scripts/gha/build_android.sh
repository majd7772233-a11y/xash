#!/bin/bash

set -e

unset ANDROID_SDK_ROOT

export JAVA_HOME=$GITHUB_WORKSPACE/java
export ANDROID_HOME=$GITHUB_WORKSPACE/sdk

export PATH=$PATH:$JAVA_HOME/bin:$ANDROID_HOME/tools:$ANDROID_HOME/tools/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/tools/bin

ANDROID_BUILD_TOOLS_VERSION="36.0.0"

pushd "$GITHUB_WORKSPACE" > /dev/null

# ---------------------------------------------------------------------------
# Android signing
# ---------------------------------------------------------------------------

if [[ -n "${MY_KEYSTORE_JKS:-}" ]]; then
	echo "Preparing Android signing keystore..."

	mkdir -p android

	printf '%s' "$MY_KEYSTORE_JKS" | base64 --decode > android/my_keystore.jks

	if [[ ! -s android/my_keystore.jks ]]; then
		echo "ERROR: Failed to decode MY_KEYSTORE_JKS"
		exit 1
	fi

	chmod 600 android/my_keystore.jks

	export MAGD_KEYSTORE_FILE="$GITHUB_WORKSPACE/android/my_keystore.jks"

	echo "Using MAGD release signing keystore."
else
	echo "MY_KEYSTORE_JKS is not available."
	echo "Gradle will use the debug signing key."
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

pushd android > /dev/null

./gradlew assembleContinuous --no-daemon

popd > /dev/null

# ---------------------------------------------------------------------------
# Prepare artifacts
# ---------------------------------------------------------------------------

mkdir -p artifacts

APK_PATH="android/app/build/outputs/apk/continuous/app-continuous.apk"
MAPPING_PATH="android/app/build/outputs/mapping/continuous"

if [[ ! -s "$APK_PATH" ]]; then
	echo "ERROR: Continuous APK was not produced."
	exit 1
fi

mv "$APK_PATH" artifacts/xash3d-fwgs-android.apk

if [[ -d "$MAPPING_PATH" ]]; then
	tar -cJvf \
		artifacts/xash3d-fwgs-android-mappings.tar.zst \
		-C "$MAPPING_PATH" \
		.
fi

# ---------------------------------------------------------------------------
# Verify APK signature
# ---------------------------------------------------------------------------

APKSIGNER="$ANDROID_HOME/build-tools/$ANDROID_BUILD_TOOLS_VERSION/apksigner"

if [[ -x "$APKSIGNER" ]]; then
	echo "Verifying Android APK signature..."

	"$APKSIGNER" verify \
		--verbose \
		artifacts/xash3d-fwgs-android.apk

	echo "APK signature verification succeeded."
else
	echo "WARNING: apksigner was not found; skipping signature verification."
fi

popd > /dev/null
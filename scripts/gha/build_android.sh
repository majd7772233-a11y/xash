#!/bin/bash

set -e

unset ANDROID_SDK_ROOT

export JAVA_HOME="$GITHUB_WORKSPACE/java"
export ANDROID_HOME="$GITHUB_WORKSPACE/sdk"

export PATH="$PATH:$JAVA_HOME/bin:$ANDROID_HOME/tools:$ANDROID_HOME/tools/bin:$ANDROID_HOME/platform-tools:$ANDROID_HOME/cmdline-tools/tools/bin"

ANDROID_BUILD_TOOLS_VERSION="36.0.0"
REQUIRE_SIGNING="${MAGD_REQUIRE_SIGNING:-0}"

pushd "$GITHUB_WORKSPACE" > /dev/null

# ---------------------------------------------------------------------------
# Android signing
# ---------------------------------------------------------------------------

if [[ "$REQUIRE_SIGNING" == "1" ]]; then
	echo "Release signing is required."

	for required_var in \
		MY_KEYSTORE_JKS \
		KEYSTORE_PASSWORD \
		KEY_ALIAS \
		KEY_PASSWORD
	do
		if [[ -z "${!required_var:-}" ]]; then
			echo "ERROR: Required signing secret is missing: $required_var"
			exit 1
		fi
	done
fi

if [[ -n "${MY_KEYSTORE_JKS:-}" ]]; then
	echo "Preparing Android signing keystore..."

	mkdir -p android

	printf '%s' "$MY_KEYSTORE_JKS" |
		base64 --decode > android/my_keystore.jks

	if [[ ! -s android/my_keystore.jks ]]; then
		echo "ERROR: Failed to decode MY_KEYSTORE_JKS"
		exit 1
	fi

	chmod 600 android/my_keystore.jks

	export MAGD_KEYSTORE_FILE="$GITHUB_WORKSPACE/android/my_keystore.jks"

	echo "Validating Android signing keystore..."

	"$JAVA_HOME/bin/keytool" \
		-list \
		-keystore "$MAGD_KEYSTORE_FILE" \
		-storepass "$KEYSTORE_PASSWORD" \
		-alias "$KEY_ALIAS" \
		> /dev/null

	echo "Android signing keystore is valid."
else
	if [[ "$REQUIRE_SIGNING" == "1" ]]; then
		echo "ERROR: Signing is required but MY_KEYSTORE_JKS is empty."
		exit 1
	fi

	echo "MY_KEYSTORE_JKS is not available."
	echo "Gradle will use the debug signing key for this non-release build."
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
		artifacts/xash3d-fwgs-android-mappings.tar.xz \
		-C "$MAPPING_PATH" \
		.
fi

# ---------------------------------------------------------------------------
# Verify APK signature
# ---------------------------------------------------------------------------

APKSIGNER="$ANDROID_HOME/build-tools/$ANDROID_BUILD_TOOLS_VERSION/apksigner"

if [[ ! -x "$APKSIGNER" ]]; then
	echo "ERROR: apksigner was not found:"
	echo "$APKSIGNER"
	exit 1
fi

echo "Verifying Android APK signature..."

"$APKSIGNER" verify \
	--verbose \
	artifacts/xash3d-fwgs-android.apk

echo "APK signature verification succeeded."

# ---------------------------------------------------------------------------
# Verify release certificate identity
# ---------------------------------------------------------------------------

if [[ "$REQUIRE_SIGNING" == "1" ]]; then
	echo "Verifying APK certificate matches the configured keystore..."

	EXPECTED_CERT="$(
		"$JAVA_HOME/bin/keytool" \
			-list \
			-v \
			-keystore "$MAGD_KEYSTORE_FILE" \
			-storepass "$KEYSTORE_PASSWORD" \
			-alias "$KEY_ALIAS" |
		awk -F': ' '/SHA256:/{print $2; exit}'
	)"

	ACTUAL_CERT="$(
		"$APKSIGNER" verify \
			--verbose \
			--print-certs \
			artifacts/xash3d-fwgs-android.apk 2>&1 |
		awk -F': ' '/Signer #1 certificate SHA-256 digest:/{print $2; exit}'
	)"

	if [[ -z "$EXPECTED_CERT" || -z "$ACTUAL_CERT" ]]; then
		echo "ERROR: Could not determine APK signing certificate."
		exit 1
	fi

	if [[ "$EXPECTED_CERT" != "$ACTUAL_CERT" ]]; then
		echo "ERROR: APK was not signed by the configured keystore."
		echo "Expected certificate: $EXPECTED_CERT"
		echo "Actual certificate:   $ACTUAL_CERT"
		exit 1
	fi

	echo "APK certificate matches the configured release keystore."
fi

popd > /dev/null
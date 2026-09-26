import com.android.build.api.dsl.ApplicationExtension
import org.gradle.kotlin.dsl.configure
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.io.File
import java.time.LocalDateTime
import java.time.Month
import java.time.temporal.ChronoUnit

plugins {
	alias(libs.plugins.android.application)
}

extensions.configure<ApplicationExtension> {
	val configuredNdkVersion = "29.0.14206865"

	val configuredNdkRoot =
		System.getenv("ANDROID_NDK_ROOT")
			?: System.getenv("ANDROID_NDK_HOME")
			?: File(
				System.getenv("ANDROID_HOME") ?: "",
				"ndk/$configuredNdkVersion"
			).path

	val magdKeystorePath =
		System.getenv("MAGD_KEYSTORE_FILE")
			?.trim()
			?.takeIf { it.isNotEmpty() }

	val magdKeystorePassword =
		System.getenv("KEYSTORE_PASSWORD")
			?.takeIf { it.isNotEmpty() }

	val magdKeyAlias =
		System.getenv("KEY_ALIAS")
			?.takeIf { it.isNotEmpty() }

	val magdKeyPassword =
		System.getenv("KEY_PASSWORD")
			?.takeIf { it.isNotEmpty() }

	val magdKeystoreFile =
		magdKeystorePath?.let { File(it) }

	val magdSigningReady =
		magdKeystoreFile?.isFile == true &&
		magdKeystorePassword != null &&
		magdKeyAlias != null &&
		magdKeyPassword != null

	namespace = "su.xash.engine"

	ndkVersion = configuredNdkVersion

	compileSdk = 35

	defaultConfig {
		applicationId = "su.xash.engine"

		versionName =
			"0.21-" + getGitHash()

		versionCode =
			getBuildNum()

		minSdk = 21
		targetSdk = 35

		buildConfigField(
			"String",
			"GIT_HASH",
			"\"${getGitHash()}\""
		)

		externalNativeBuild {
			val engineRoot =
				projectDir.parentFile.parent

			experimentalProperties[
				"ninja.abiFilters"
			] = setOf(
				"armeabi-v7a",
				"arm64-v8a",
				"x86"
			)

			experimentalProperties[
				"ninja.path"
			] = File(
				engineRoot,
				"wscript"
			).path

			experimentalProperties[
				"ninja.configure"
			] = "run-python"

			experimentalProperties[
				"ninja.arguments"
			] = setOf(
				File(
					engineRoot,
					"scripts/configure-ninja.py"
				).path,

				engineRoot,

				"--variant=\${ndk.variantName}",

				"--abi=\${ndk.abi}",

				"--configuration-dir=\${ndk.buildRoot}",

				"--ndk-version=\${ndk.moduleNdkVersion}",

				"--min-sdk-version=\${ndk.minPlatform}",

				"--ndk-root=$configuredNdkRoot",

				"-p:Configuration=\${ndk.variantName}",

				"-p:Platform=\${ndk.abi}"
			)
		}
	}

	compileOptions {
		sourceCompatibility =
			JavaVersion.VERSION_11

		targetCompatibility =
			JavaVersion.VERSION_11
	}

	kotlin {
		compilerOptions {
			jvmTarget =
				JvmTarget.JVM_11
		}
	}

	buildFeatures {
		viewBinding = true
		buildConfig = true
	}

	signingConfigs {
		create("androidDebugKey") {
			storeFile =
				File(
					projectDir.parentFile,
					"debug.keystore"
				)

			storePassword = "android"
			keyAlias = "androiddebugkey"
			keyPassword = "android"
		}

		if (magdSigningReady) {
			create("magdRelease") {
				storeFile = magdKeystoreFile!!

				storePassword = magdKeystorePassword!!
				keyAlias = magdKeyAlias!!
				keyPassword = magdKeyPassword!!
			}
		}
	}

	lint {
		abortOnError = false
	}

	packaging {
		jniLibs {
			keepDebugSymbols.add("**/*.so")
			useLegacyPackaging = true
		}
	}

	sourceSets {
		getByName("main") {
			assets.directories.add(
				"../../3rdparty/extras/xash-extras"
			)

			java.directories.add(
				"../../3rdparty/SDL/android-project/app/src/main/java"
			)
		}
	}

	buildTypes {
		debug {
			isMinifyEnabled = false
			isShrinkResources = false
			isDebuggable = true

			applicationIdSuffix = ".test"

			proguardFiles(
				getDefaultProguardFile(
					"proguard-android-optimize.txt"
				),
				"proguard-rules.pro"
			)

			buildConfigField(
				"boolean",
				"ENABLE_AUTO_UPDATE",
				"false"
			)
		}

		release {
			isMinifyEnabled = true
			isShrinkResources = true

			proguardFiles(
				getDefaultProguardFile(
					"proguard-android-optimize.txt"
				),
				"proguard-rules.pro"
			)

			buildConfigField(
				"boolean",
				"ENABLE_AUTO_UPDATE",
				"false"
			)

			if (magdSigningReady) {
				signingConfig =
					signingConfigs.getByName(
						"magdRelease"
					)
			}
		}

		register("asan") {
			initWith(
				getByName("debug")
			)

			signingConfig =
				signingConfigs.getByName(
					"androidDebugKey"
				)
		}

		register("continuous") {
			initWith(
				getByName("release")
			)

			applicationIdSuffix = ".test"

			buildConfigField(
				"boolean",
				"ENABLE_AUTO_UPDATE",
				"true"
			)

			if (magdSigningReady) {
				signingConfig =
					signingConfigs.getByName(
						"magdRelease"
					)
			} else {
				signingConfig =
					signingConfigs.getByName(
						"androidDebugKey"
					)
			}
		}
	}
}

dependencies {
	implementation(
		libs.material
	)

	implementation(
		libs.appcompat
	)

	implementation(
		libs.navigation.runtime.ktx
	)

	implementation(
		libs.navigation.fragment.ktx
	)

	implementation(
		libs.navigation.ui.ktx
	)

	implementation(
		libs.preference.ktx
	)

	implementation(
		libs.swiperefreshlayout
	)

	implementation(
		libs.acra.http
	)
}

fun getBuildNum(): Int {
	val now = LocalDateTime.now()

	val releaseDate =
		LocalDateTime.of(
			2015,
			Month.APRIL,
			1,
			0,
			0,
			0
		)

	val qBuildNum =
		releaseDate.until(
			now,
			ChronoUnit.DAYS
		)

	val minuteOfDay =
		now.hour * 60 + now.minute

	return (
		qBuildNum * 10000 +
		minuteOfDay
	).toInt()
}

fun getGitHash(): String {
	val process =
		ProcessBuilder(
			"git",
			"rev-parse",
			"--short",
			"HEAD"
		)
			.directory(project.rootDir)
			.redirectErrorStream(true)
			.start()

	return process
		.inputStream
		.bufferedReader()
		.readText()
		.trim()
}
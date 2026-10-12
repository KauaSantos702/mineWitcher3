@echo off
rem Runs the Gradle wrapper on a Java 25 JDK (Minecraft 26.x / Loom 1.18 need it), without touching
rem %USERPROFILE%\.gradle\gradle.properties (which pins org.gradle.java.home to JDK 21 for other projects).
rem Usage: gradle25.cmd build      (any gradlew arguments)
rem Override the JDK with MW3_JDK=<path>.
setlocal
set "JDK=%MW3_JDK%"
if not defined JDK for /d %%D in ("C:\Program Files\Eclipse Adoptium\jdk-25*") do set "JDK=%%~D"
if not defined JDK for /d %%D in ("%LOCALAPPDATA%\Programs\Temurin\jdk-25*") do set "JDK=%%~D"
if not exist "%JDK%\bin\java.exe" (
	echo No JDK 25 found. Install Temurin 25 or set MW3_JDK. 1>&2
	exit /b 1
)
set "JAVA_HOME=%JDK%"
call "%~dp0gradlew.bat" "-Dorg.gradle.java.home=%JDK%" %*

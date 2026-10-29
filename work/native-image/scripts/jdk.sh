# jdk.sh — THE JAVA_HOME resolver (M3 stage C, 2026-10-12). Sourced by inst.sh and by every script that runs
# $G/bin/{java,javac,native-image}. Picks the GraalVM by Minecraft version; an explicit G=... still wins.
#   MCV unset / 26.x    -> Oracle GraalVM for JDK 25.0.4+7.1    (26.x needs Java 25: class files major 69)
#   MCV=1.*             -> Oracle GraalVM for JDK 21.0.12+7.1   (1.21.5 + 1.21.11)
# Both live in work/native-image/graalvm/ (PROVENANCE.md rows 1-2). Exports GJDK (21|25) for scripts that care.
_W="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case ${MCV:-26.2} in
  1.*) GJDK=21; _G=$_W/graalvm/graalvm-jdk-21.0.12+7.1/Contents/Home;;
  *)   GJDK=25; _G=$_W/graalvm/graalvm-jdk-25.0.4+7.1/Contents/Home;;
esac
G=${G:-$_G}
unset _W _G

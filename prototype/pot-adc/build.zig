const std = @import("std");

pub fn build(b: *std.Build) void {
    // STM32L452: Cortex-M4. No floating point needed: millivolts are computed with integers.
    const target = b.resolveTargetQuery(.{
        .cpu_arch = .thumb,
        .os_tag = .freestanding,
        .abi = .eabi,
        .cpu_model = .{ .explicit = &std.Target.arm.cpu.cortex_m4 },
    });
    const optimize = b.option(std.builtin.OptimizeMode, "optimize", "Optimization mode") orelse .small;

    const exe = b.addExecutable(.{
        .name = "pot-adc.elf",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            // keep symbols so a debugger can find the dbg_* variables
            .strip = false,
        }),
    });
    exe.setLinkerScript(b.path("link.ld"));
    exe.entry = .{ .symbol_name = "resetHandler" };
    b.installArtifact(exe);

    // `zig build flash`: build, then program the Nucleo with OpenOCD
    const flash = b.addSystemCommand(&.{ "openocd", "-f", "interface/stlink.cfg", "-f", "target/stm32l4x.cfg", "-c" });
    flash.addPrefixedFileArg("program ", exe.getEmittedBin());
    flash.addArgs(&.{ "-c", "reset run", "-c", "shutdown" });
    b.step("flash", "Build and flash the board").dependOn(&flash.step);
}

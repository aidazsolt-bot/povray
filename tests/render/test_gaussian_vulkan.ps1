param(
    [Parameter(Mandatory = $true)]
    [string] $Renderer,
    [string] $ReferenceRenderer
)

$ErrorActionPreference = 'Stop'
$Renderer = (Resolve-Path -LiteralPath $Renderer).Path
if ($ReferenceRenderer) { $ReferenceRenderer = (Resolve-Path -LiteralPath $ReferenceRenderer).Path }
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('povray-vulkan-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
Add-Type -AssemblyName System.Drawing
$oldDrivers = $env:VK_DRIVER_FILES
$oldIcd = $env:VK_ICD_FILENAMES

function Render([string] $name, [string[]] $options) {
    $args = @('+Itest.pov', '+W64', '+H64', '-D', '-P', '+FN', '+Q9',
              'Stochastic_Seed=1', 'High_Reproducibility=on', "+O$name.png") + $options
    $process = Start-Process -FilePath $Renderer -ArgumentList $args -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput "$name.stdout.log" -RedirectStandardError "$name.stderr.log"
    if ($process.ExitCode -ne 0) {
        throw "Render $name failed: $(Get-Content "$name.stderr.log" -Raw)"
    }
    $log = Get-Content "$name.stderr.log" -Raw
    if ($log -notmatch 'POV-Ray finished' -or $log -match 'Parse Error|Fatal error') {
        throw "Render $name did not complete successfully: $log"
    }
    return $log
}

function Pixels([string] $path) {
    $image = [System.Drawing.Bitmap]::FromFile((Join-Path $work $path))
    try {
        $result = New-Object 'System.Collections.Generic.List[int]'
        for ($y = 0; $y -lt $image.Height; $y++) {
            for ($x = 0; $x -lt $image.Width; $x++) {
                $result.Add($image.GetPixel($x, $y).ToArgb())
            }
        }
        return ,$result.ToArray()
    }
    finally { $image.Dispose() }
}

function SamePixels([string] $a, [string] $b) {
    $pa = Pixels $a
    $pb = Pixels $b
    if ($pa.Length -ne $pb.Length) { throw "Different dimensions: $a / $b" }
    for ($i = 0; $i -lt $pa.Length; $i++) {
        if ($pa[$i] -ne $pb[$i]) { throw "Pixel $i differs: $a / $b" }
    }
}

Push-Location $work
try {
    @'
ply
format ascii 1.0
element vertex 4
property float x
property float y
property float z
property float f_dc_0
property float f_dc_1
property float f_dc_2
property float opacity
property float scale_0
property float scale_1
property float scale_2
property float rot_0
property float rot_1
property float rot_2
property float rot_3
end_header
-0.2 0 0 1 0 0 2 -2 -1.8 -2.5 0.9238795 0 0.3826834 0
0.25 0 0.15 0 1 0 2 -2.5 -1.5 -2 0.5 0.5 0.5 0.5
0.1 0.2 0.4 0 0 1 1.5 -3 -2 -2.2 2 -0.5 0.1 0.7
0 -0.2 0.2 1 1 0 2 -2 -80 -3 0.7071068 0 0 0.7071068
'@ | Set-Content -LiteralPath model.ply -Encoding Ascii
    @'
#version 3.8;
global_settings { assumed_gamma 1.0 }
#ifndef (EmptyCloud) #declare EmptyCloud = 0; #end
#ifndef (CpuBackend) #declare CpuBackend = 0; #end
#ifndef (RelightMode) #declare RelightMode = 0; #end
#ifndef (SampleMode) #declare SampleMode = 2; #end
#ifndef (MultiCloud) #declare MultiCloud = 0; #end
camera {
  location <sin(clock * pi), 0, -3>
  look_at <0, 0, 0>
  right x
  angle 45
}
background { color rgb <0.02, 0.03, 0.04> }
#declare TestCloud = gaussian_splat {
  "model.ply"
  #if (SampleMode = 2) khr_ellipse #end
  samples SampleMode
  #if (CpuBackend) backend cpu #else backend vulkan #end
  #if (EmptyCloud) opacity_cutoff 1 #end
  #if (RelightMode)
    lighting_mode relight
    texture {
      pigment { rgb 1 }
      finish { ambient 0.18 diffuse 0.82 specular 0.22 roughness 0.035 reflection 0.04 }
    }
  #else
    lighting_mode khr
  #end
}
union {
  object { TestCloud }
  #if (MultiCloud)
    object { TestCloud translate <0.5, 0.2, 0.5> }
  #end
  sphere { <100, 100, 100>, 0.1 pigment { rgb 0 } }
}
'@ | Set-Content -LiteralPath test.pov -Encoding Ascii

    $single = Render 'single' @('+WT1')
    $multi = Render 'multi' @('+WT4')
    foreach ($log in @($single, $multi)) {
        if ([regex]::Matches($log, 'gaussian_splat vulkan frame:').Count -ne 1 -or
            $log -match 'using CPU projection') {
            throw 'Expected exactly one successful GPU dispatch without CPU fallback'
        }
    }
    SamePixels 'single.png' 'multi.png'
    $clouds = Render 'clouds' @('+WT4', 'Declare=MultiCloud=1')
    if ([regex]::Matches($clouds, 'gaussian_splat vulkan frame:').Count -ne 2) {
        throw 'Two cloud copies must each produce one independent GPU frame'
    }
    $pixels = Pixels 'single.png'
    if ($pixels[32 * 64 + 32] -eq $pixels[0]) { throw 'Visible splats were not rendered' }

    $empty = Render 'empty' @('+WT4', 'Declare=EmptyCloud=1')
    $emptyPixels = Pixels 'empty.png'
    foreach ($pixel in $emptyPixels) {
        if ($pixel -ne $emptyPixels[0]) { throw 'An empty cloud must show only the background' }
    }
    if ($empty -match 'using CPU projection') { throw 'Empty GPU cloud incorrectly fell back to CPU' }

    $animation = Render 'orbit' @('+WT4', '+KFI1', '+KFF2', '+KI0', '+KF0.5')
    if ([regex]::Matches($animation, 'gaussian_splat vulkan frame:').Count -ne 2) {
        throw 'Animation must prepare one GPU frame per camera'
    }
    $resized = Render 'resized' @('+WT4', '+W80', '+H48')
    if ($resized -notmatch 'vulkan frame: 80x48') { throw 'Wrong GPU framebuffer dimensions' }
    [void](Render 'cpu' @('+WT4', 'Declare=CpuBackend=1'))
    foreach ($samples in @(1, 2, 4)) {
        $options = @('+WT1', 'Declare=CpuBackend=1', 'Declare=RelightMode=1', "Declare=SampleMode=$samples")
        $log = Render "relight$samples" $options
        if ($samples -eq 2 -and $log -notmatch 'Reflected Rays:') {
            throw 'Relight regression must exercise secondary rays'
        }
        if ($ReferenceRenderer) {
            $currentRenderer = $Renderer
            try {
                $Renderer = $ReferenceRenderer
                [void](Render "reference$samples" $options)
            }
            finally { $Renderer = $currentRenderer }
            SamePixels "relight$samples.png" "reference$samples.png"
        }
    }

    $env:VK_DRIVER_FILES = Join-Path $work 'nonexistent-driver.json'
    $env:VK_ICD_FILENAMES = $env:VK_DRIVER_FILES
    $fallback = Render 'fallback' @('+WT4')
    if ([regex]::Matches($fallback, 'initialization unavailable; using CPU projection').Count -ne 1) {
        throw 'Failed Vulkan initialization must be diagnosed once, not retried per pixel/thread'
    }
    SamePixels 'cpu.png' 'fallback.png'
    Write-Output 'PASS: GPU dispatch, transparent/empty pixels, nested cloud, thread consistency, animation, dimensions and CPU fallback'
}
finally {
    $env:VK_DRIVER_FILES = $oldDrivers
    $env:VK_ICD_FILENAMES = $oldIcd
    Pop-Location
    Remove-Item -LiteralPath $work -Recurse -Force
}
